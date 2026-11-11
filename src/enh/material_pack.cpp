#include "enh/material_pack.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <set>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "d3d9/device.h"
#include "engine/materials.h"
#include "res/dds.h"

namespace rvk::enh::matpack {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

void getFloat(const toml::table& t, std::string_view key, std::optional<float>& out, std::vector<std::string>& warn, std::string_view where) {
    const toml::node* n = t.get(key);
    if (n == nullptr) return;
    if (const auto v = n->value<double>()) {
        out = static_cast<float>(*v);
    } else {
        warn.push_back(std::string(where) + ": '" + std::string(key) + "' is not a number");
    }
}

Constants readConstants(const toml::table& t, std::vector<std::string>& warn, std::string_view where) {
    Constants c;
    getFloat(t, "roughness_scale", c.roughnessScale, warn, where);
    getFloat(t, "roughness_bias", c.roughnessBias, warn, where);
    getFloat(t, "roughness", c.roughness, warn, where);
    getFloat(t, "metalness", c.metalness, warn, where);
    getFloat(t, "emissive", c.emissive, warn, where);
    getFloat(t, "ior", c.ior, warn, where);
    getFloat(t, "transmission", c.transmission, warn, where);
    getFloat(t, "wetness", c.wetness, warn, where);
    if (const toml::node* n = t.get("map")) {
        if (const auto s = n->value<std::string>()) c.map = *s;
    }
    for (const auto& [k, v] : t) {
        static constexpr std::string_view known[] = {"roughness_scale", "roughness_bias", "roughness", "metalness", "emissive", "ior", "transmission", "wetness", "map"};
        if (std::find(std::begin(known), std::end(known), k.str()) == std::end(known)) {
            warn.push_back(std::string(where) + ": unknown key '" + std::string(k.str()) + "'");
        }
    }
    return c;
}

}  // namespace

// ---- names ---------------------------------------------------------------------------------------------------------------------

std::string normalizeKey(std::string_view name) {
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
    std::string s = lower(name);
    for (;;) {
        bool cut = false;
        for (const std::string_view ext : {".dds", ".png", "._ximg", ".ximg", ".tga"}) {
            if (endsWith(s, ext)) {
                s.resize(s.size() - ext.size());
                cut = true;
            }
        }
        if (!cut) break;
    }
    return s;
}

FileName classifyFile(std::string_view fileName) {
    std::string k = normalizeKey(fileName);
    FileName f;
    static constexpr std::pair<std::string_view, Kind> suffixes[] = {
        {"_orm", Kind::Orm}, {"_roughness", Kind::Roughness}, {"_rough", Kind::Roughness}, {"_r", Kind::Roughness}};
    for (const auto& [suf, kind] : suffixes) {
        if (k.size() > suf.size() && endsWith(k, suf)) {
            f.kind = kind;
            k.resize(k.size() - suf.size());
            break;
        }
    }
    f.key = std::move(k);
    return f;
}

bool globMatch(std::string_view pattern, std::string_view text) {
    // iterative wildcard match with one backtrack point
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    const auto eq = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || (pattern[p] != '*' && eq(pattern[p], text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// ---- constants / manifest ----------------------------------------------------------------------------------------------------------

void Constants::overlay(const Constants& o) {
    const auto take = [](auto& dst, const auto& src) {
        if (src) dst = src;
    };
    take(roughnessScale, o.roughnessScale);
    take(roughnessBias, o.roughnessBias);
    take(roughness, o.roughness);
    take(metalness, o.metalness);
    take(emissive, o.emissive);
    take(ior, o.ior);
    take(transmission, o.transmission);
    take(wetness, o.wetness);
    take(map, o.map);
}

std::optional<Manifest> parseManifest(std::string_view text, std::string* error) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        if (error != nullptr) *error = std::string(e.description());
        return std::nullopt;
    }
    Manifest m;
    if (const toml::table* d = tbl["defaults"].as_table()) m.defaults = readConstants(*d, m.warnings, "[defaults]");
    if (const toml::table* mats = tbl["material"].as_table()) {
        for (const auto& [name, node] : *mats) {
            const toml::table* t = node.as_table();
            if (t == nullptr) {
                m.warnings.push_back("[material] '" + std::string(name.str()) + "' is not a table");
                continue;
            }
            Rule r;
            r.pattern = lower(name.str());
            // a name written with its extension / suffix still matches
            if (r.pattern.find('*') == std::string::npos && r.pattern.find('?') == std::string::npos) r.pattern = normalizeKey(r.pattern);
            r.c = readConstants(*t, m.warnings, "[material." + std::string(name.str()) + "]");
            m.rules.push_back(std::move(r));
        }
    }
    for (const auto& [k, v] : tbl) {
        if (k.str() != "defaults" && k.str() != "material") m.warnings.push_back("unknown table '" + std::string(k.str()) + "'");
    }
    return m;
}

bool hasRule(const Manifest& m, std::string_view key) {
    return std::any_of(m.rules.begin(), m.rules.end(), [&](const Rule& r) { return globMatch(r.pattern, key); });
}

Constants resolveConstants(const Manifest& m, std::string_view key) {
    Constants c = m.defaults;
    for (const Rule& r : m.rules) {
        if (globMatch(r.pattern, key)) c.overlay(r.c);
    }
    return c;
}

// ---- the pack word ----------------------------------------------------------------------------------------------------------------------

std::uint32_t encodeWord(std::uint32_t slot, bool orm, const Constants& c, float defaultRoughness) {
    const bool map = slot != 0;
    std::uint32_t w = (slot & kWordSlotMask) | kWordPresent;
    float rs = c.roughnessScale.value_or(1.0f), rb = c.roughnessBias.value_or(0.0f);
    if (!map) {
        rs = 0.0f;
        rb = c.roughness.value_or(defaultRoughness);
    }  // (with a map the constant roughness is ignored: wildcard rules must not flatten a map; use roughness_scale / roughness_bias)
    float metal = c.metalness.value_or(map && orm ? 1.0f : 0.0f);
    const auto q = [](float v, float scale, float top) { return static_cast<std::uint32_t>(std::lround(std::clamp(v * scale, 0.0f, top))); };
    w |= q(rs, 8.0f, 15.0f) << 16;
    w |= q(rb, 15.0f, 15.0f) << 20;
    w |= q(metal, 15.0f, 15.0f) << 24;
    w |= q(c.wetness.value_or(0.0f), 3.0f, 3.0f) << 28;
    if (c.transmission.value_or(0.0f) > 0.0f) w |= kWordGlass;
    if (orm && map) w |= kWordOrm;
    if (orm && map && c.emissive.value_or(0.0f) > 0.0f) w |= kWordEmissive;
    return w;
}

float roughnessFromPower(float specularPower) {
    const float n = std::max(specularPower, 0.0f) * 128.0f;
    return std::sqrt(std::sqrt(2.0f / (n + 2.0f)));
}

float glossGate(float r) {
    const float t = clamp01((r - kGlossStart) / kGlossSpan);
    return r + (1.0f - r) * t;
}

Derived deriveFromSpecular(const SpecularInfo& s, const DeriveOptions& o, float defaultRoughness) {
    Derived d;
    if (!o.enabled || !s.hasMask) return d;
    const float strength = s.maskMap ? kMapMaskStrength : clamp01(s.maskLum);
    if (!s.maskMap && strength < 0.02f) return d;  // a black constant mask: no highlight at all
    const float lo = std::min(o.minRoughness, o.maxRoughness), hi = std::max(o.minRoughness, o.maxRoughness);
    float r = s.powerKnown ? roughnessFromPower(s.power) : defaultRoughness;  // a power TEXTURE: its mean is unknown, the default roughness stands in
    r = glossGate(std::clamp(r, lo, hi));
    d.valid = true;
    d.strength = strength;
    d.roughness = clamp01(r + (1.0f - r) * (1.0f - strength));
    return d;
}

std::uint32_t encodeDerivedWord(float roughness) {
    return kWordPresent | kWordDerived | (static_cast<std::uint32_t>(std::lround(clamp01(roughness) * 255.0f)) << 16);
}

SpecularInfo specularInfo(const engine::materials::ShaderInfo& info) {
    namespace mats = engine::materials;
    SpecularInfo s;
    if (!info.enableSpecular || info.specular.source == mats::Source::None) return s;
    // a Diffuse that is a CUBE sampler (the wet beach / rock "_Specular" layers, 19 materials): the layer IS the engine's own environment reflection, drawn blended over the
    // ground; adding another sky reflection on top would double it, so these are never derived
    if (info.diffuse.source == mats::Source::Cube) return s;
    const mats::Role& m = info.specular;
    if (m.source == mats::Source::Sampler) {
        s.hasMask = true;
        s.maskMap = true;
    } else if (m.hasConstant) {
        s.hasMask = true;
        s.maskLum = 0.3f * m.constant[0] + 0.59f * m.constant[1] + 0.11f * m.constant[2];
        if (!std::isfinite(s.maskLum)) s.maskLum = 1.0f;
    }
    const mats::Role& p = info.specularPower;
    if (p.source == mats::Source::None) {
        s.powerKnown = true;  // the engine's default m_fSpecularPower = 1
        s.power = 1.0f;
    } else if (p.hasConstant) {
        const int c = p.swizzle >= 3 && p.swizzle <= 5 ? p.swizzle - 2 : 0;  // "", .rgb, .r, .g, .b, .a
        const float v = p.constant[c];
        s.powerKnown = std::isfinite(v) && v >= 0.0f && v <= 64.0f;  // (a value outside the engine's range = not read right: the default roughness stands in)
        s.power = v;
    }
    return s;
}

Decoded decodeWord(std::uint32_t w) {
    Decoded d;
    d.slot = w & kWordSlotMask;
    d.present = (w & kWordPresent) != 0;
    d.derived = d.slot == 0 && (w & kWordDerived) != 0;
    d.orm = (w & kWordOrm) != 0 && !d.derived;
    d.emissive = (w & kWordEmissive) != 0;
    d.glass = (w & kWordGlass) != 0;
    d.roughnessScale = static_cast<float>((w >> 16) & 15u) / 8.0f;
    d.roughnessBias = static_cast<float>((w >> 20) & 15u) / 15.0f;
    if (d.derived) {
        d.roughnessScale = 0.0f;
        d.roughnessBias = static_cast<float>((w >> 16) & 255u) / 255.0f;
    }
    d.metalness = static_cast<float>((w >> 24) & 15u) / 15.0f;
    d.wetness = static_cast<float>((w >> 28) & 3u) / 3.0f;
    return d;
}

Surface evaluate(const Decoded& d, const float t[4]) {
    Surface s;
    const bool map = d.slot != 0;
    const float texRough = !map ? 1.0f : d.orm ? t[1] : t[0];
    s.roughness = clamp01(texRough * d.roughnessScale + d.roughnessBias);
    s.occlusion = map && d.orm ? t[0] : 1.0f;
    s.metalness = clamp01(map && d.orm ? t[2] * d.metalness : d.metalness);
    s.emissive = map && d.orm && d.emissive ? t[3] : 0.0f;
    return s;
}

// ---- the loader ----------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kFmtA8R8G8B8 = 21, kFmtL8 = 50;

bool readFile(const std::filesystem::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

struct Decoded8 {
    std::uint32_t w = 0, h = 0, channels = 0;  // 1 = L8, 4 = BGRA
    std::vector<std::vector<std::uint8_t>> mips;
};

// box-filtered mip chain of tightly packed 1 or 4 channel 8 bit data
void buildMips(Decoded8& d, std::vector<std::uint8_t> base) {
    d.mips.clear();
    d.mips.push_back(std::move(base));
    std::uint32_t w = d.w, h = d.h;
    while (w > 1 || h > 1) {
        const std::uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const auto& src = d.mips.back();
        std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * nh * d.channels);
        for (std::uint32_t y = 0; y < nh; ++y) {
            for (std::uint32_t x = 0; x < nw; ++x) {
                const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                for (std::uint32_t c = 0; c < d.channels; ++c) {
                    const auto at = [&](std::uint32_t xx, std::uint32_t yy) { return static_cast<std::uint32_t>(src[(static_cast<std::size_t>(yy) * w + xx) * d.channels + c]); };
                    dst[(static_cast<std::size_t>(y) * nw + x) * d.channels + c] = static_cast<std::uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                }
            }
        }
        d.mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

bool decodePng(const std::vector<std::uint8_t>& file, Kind kind, Decoded8& out) {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &n, 4);
    if (px == nullptr || w <= 0 || h <= 0) return false;
    out.w = static_cast<std::uint32_t>(w);
    out.h = static_cast<std::uint32_t>(h);
    const std::size_t count = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> base;
    if (kind == Kind::Roughness) {
        out.channels = 1;
        base.resize(count);
        for (std::size_t i = 0; i < count; ++i) base[i] = px[i * 4];  // R (a grey PNG has R = G = B)
    } else {
        out.channels = 4;
        base.resize(count * 4);
        for (std::size_t i = 0; i < count; ++i) {  // RGBA -> D3D9 A8R8G8B8 (B, G, R, A in memory)
            base[i * 4 + 0] = px[i * 4 + 2];
            base[i * 4 + 1] = px[i * 4 + 1];
            base[i * 4 + 2] = px[i * 4 + 0];
            base[i * 4 + 3] = px[i * 4 + 3];
        }
    }
    stbi_image_free(px);
    buildMips(out, std::move(base));
    return true;
}

}  // namespace

namespace {
std::function<bool()> g_reload;
}
void setReloadHook(std::function<bool()> hook) { g_reload = std::move(hook); }
bool requestReload() { return g_reload && g_reload(); }

MaterialPack::MaterialPack(d3d9::Device& device) : dev_(device) {}

MaterialPack::~MaterialPack() {
    loadToken_.fetch_add(1);
    if (worker_.joinable()) worker_.join();
}

void MaterialPack::configure(bool enabled, std::vector<std::filesystem::path> roots, std::uint64_t budgetBytes, float defaultRoughness) {
    enabled_ = enabled;
    const std::uint32_t token = loadToken_.fetch_add(1) + 1;
    if (worker_.joinable()) worker_.join();
    if (!enabled || roots.empty()) {
        const std::lock_guard lock(mutex_);
        pending_ = std::make_shared<const Snapshot>();
        hasPending_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_release);
        return;
    }
    worker_ = std::thread([this, roots = std::move(roots), budgetBytes, defaultRoughness, token]() mutable { load(std::move(roots), budgetBytes, defaultRoughness, token); });
}

void MaterialPack::load(std::vector<std::filesystem::path> roots, std::uint64_t budget, float defaultRoughness, std::uint32_t token) {
    namespace fs = std::filesystem;
    const auto t0 = std::chrono::steady_clock::now();
    auto snap = std::make_shared<Snapshot>();
    snap->defaultRoughness = defaultRoughness;
    struct Found {
        fs::path orm, rough;
    };
    std::unordered_map<std::string, Found> found;
    std::vector<std::pair<fs::path, Manifest>> manifests;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            spdlog::warn("enh: materials: pack folder '{}' does not exist", root.string());
            continue;
        }
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            const std::string ext = lower(p.extension().string());
            if (ext == ".toml") {
                const std::string name = lower(p.filename().string());
                if (name != "pack.toml" && name != "manifest.toml") continue;
                std::vector<std::uint8_t> bytes;
                std::string err;
                if (!readFile(p, bytes)) continue;
                auto m = parseManifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &err);
                if (!m) {
                    spdlog::warn("enh: materials: {}: {}", p.string(), err);
                    continue;
                }
                for (const std::string& w : m->warnings) spdlog::warn("enh: materials: {}: {}", p.string(), w);
                manifests.emplace_back(p.parent_path(), std::move(*m));
                continue;
            }
            if (ext != ".dds" && ext != ".png") continue;
            const FileName fn = classifyFile(p.filename().string());
            if (fn.kind == Kind::Unknown) continue;
            ++snap->stats.files;
            (fn.kind == Kind::Orm ? found[fn.key].orm : found[fn.key].rough) = p;  // later roots override
        }
    }
    // merge manifests: rules in discovery order (roots in order)
    for (auto& [dir, m] : manifests) {
        snap->manifest.defaults.overlay(m.defaults);
        for (Rule& r : m.rules) {
            if (r.c.map) r.c.map = (dir / *r.c.map).string();  // relative to the manifest
            snap->manifest.rules.push_back(std::move(r));
        }
    }
    snap->stats.rules = static_cast<std::uint32_t>(snap->manifest.rules.size());
    // explicit maps of the manifest become files of their rule's key (exact patterns only)
    for (const Rule& r : snap->manifest.rules) {
        if (!r.c.map || r.pattern.find_first_of("*?") != std::string::npos) continue;
        const FileName fn = classifyFile(*r.c.map);
        Found& f = found[r.pattern];
        (fn.kind == Kind::Roughness ? f.rough : f.orm) = *r.c.map;
    }

    snap->budget = budget;
    for (auto& [key, f] : found) {
        if (loadToken_.load() != token) return;  // superseded
        const bool orm = !f.orm.empty();
        const fs::path& file = orm ? f.orm : f.rough;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            ++snap->stats.failed;
            spdlog::warn("enh: materials: cannot read '{}'", file.string());
            continue;
        }
        Entry e;
        e.orm = orm;
        e.file = file.string();
        e.c = resolveConstants(snap->manifest, key);
        snap->entries.emplace(key, std::move(e));
        ++snap->stats.loaded;
    }
    snap->stats.entries = static_cast<std::uint32_t>(snap->entries.size());
    snap->stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (loadToken_.load() != token) return;
    spdlog::info("enh: materials: pack indexed: {} files, {} entries ({} failed), {} rules, {:.0f} ms (maps are decoded when a draw first uses them)", snap->stats.files,
                 snap->stats.loaded, snap->stats.failed, snap->stats.rules, snap->stats.ms);
    const std::lock_guard lock(mutex_);
    pending_ = std::move(snap);
    hasPending_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
}

void MaterialPack::setDerive(const DeriveOptions& o) {
    derive_ = o;
    generation_.fetch_add(1, std::memory_order_release);
}

std::uint32_t MaterialPack::deriveWord(const SpecularInfo& s) const {
    if (!derive_.enabled) return 0;
    const Derived d = deriveFromSpecular(s, derive_, snap_ ? snap_->defaultRoughness : 0.7f);
    return d.valid ? encodeDerivedWord(d.roughness) : 0u;
}

Stats MaterialPack::stats() const { return snap_ ? snap_->stats : Stats{}; }

std::unique_ptr<d3d9::Texture> MaterialPack::createMap(const Entry& e, const Snapshot& s, std::uint64_t& bytes) {
    namespace fs = std::filesystem;
    std::vector<std::uint8_t> data;
    const fs::path file = e.file;
    if (!readFile(file, data)) {
        spdlog::warn("enh: materials: cannot read '{}'", e.file);
        return nullptr;
    }
    d3d9::TextureDesc d;
    std::unique_ptr<d3d9::Texture> tex;
    std::uint64_t size = 0;
    if (lower(file.extension().string()) == ".dds") {
        std::string err;
        const auto dds = res::parseDds(data, &err);
        if (!dds || dds->cube || dds->volume) {
            spdlog::warn("enh: materials: '{}': {}", e.file, dds ? "cube / volume maps are not supported" : err);
            return nullptr;
        }
        for (std::uint32_t m = 0; m < dds->mips; ++m) {
            if (const auto l = dds->level(0, m)) size += l->bytes.size();
        }
        if (s.used + size > s.budget) {
            spdlog::warn("enh: materials: memory budget reached, '{}' skipped", e.file);
            return nullptr;
        }
        d.width = dds->width;
        d.height = dds->height;
        d.levels = dds->mips;
        d.format = dds->d3dFormat;
        tex = dev_.createTexture(d);
        if (!tex) {
            spdlog::warn("enh: materials: '{}': unsupported format {}", e.file, dds->d3dFormat);
            return nullptr;
        }
        for (std::uint32_t m = 0; m < dds->mips; ++m) {
            if (const auto l = dds->level(0, m)) tex->setLevelData(m, 0, l->bytes, l->rowPitch);
        }
    } else {
        Decoded8 img;
        if (!decodePng(data, e.orm ? Kind::Orm : Kind::Roughness, img)) {
            spdlog::warn("enh: materials: '{}': not a readable PNG", e.file);
            return nullptr;
        }
        for (const auto& m : img.mips) size += m.size();
        if (s.used + size > s.budget) {
            spdlog::warn("enh: materials: memory budget reached, '{}' skipped", e.file);
            return nullptr;
        }
        d.width = img.w;
        d.height = img.h;
        d.levels = static_cast<std::uint32_t>(img.mips.size());
        d.format = img.channels == 1 ? kFmtL8 : kFmtA8R8G8B8;
        tex = dev_.createTexture(d);
        if (!tex) return nullptr;
        for (std::uint32_t m = 0; m < img.mips.size(); ++m) tex->setLevelData(m, 0, img.mips[m], std::max(1u, img.w >> m) * img.channels);
    }
    bytes = size;
    return tex;
}

MaterialPack::Hit MaterialPack::resolve(const Snapshot& s, std::string_view key) {
    Hit h;
    if (const auto it = s.entries.find(std::string(key)); it != s.entries.end()) {
        const Entry& e = it->second;
        if (!e.tried) {
            e.tried = true;
            std::uint64_t bytes = 0;
            e.texture = createMap(e, s, bytes);
            s.used += bytes;
            if (e.texture) spdlog::info("enh: materials: map of '{}' loaded ({:.1f} MiB of the pack's {:.0f} MiB budget)", key, static_cast<double>(s.used) / (1024.0 * 1024.0),
                                        static_cast<double>(s.budget) / (1024.0 * 1024.0));
        }
        h.texture = e.texture.get();
        // (a map that failed to load: the entry's constants alone)
        h.word = encodeWord(h.texture != nullptr ? 1 : 0, e.orm, e.c, s.defaultRoughness) & ~kWordSlotMask;  // the bindless slot is added at lookup
        return h;
    }
    // a wildcard / exact rule without a map of its own: constants only (roughness / metalness for every matching material)
    if (hasRule(s.manifest, key)) {
        const Constants c = resolveConstants(s.manifest, key);
        if (c.roughness || c.metalness || c.roughnessBias || c.wetness || c.transmission) h.word = encodeWord(0, false, c, s.defaultRoughness);
    }
    return h;
}

bool MaterialPack::adopt() {
    // per draw: no mutex unless the loader published a snapshot (cpuclean: the unlock was 0.8 % of the thread)
    if (hasPending_.load(std::memory_order_acquire)) {
        const std::lock_guard lock(mutex_);
        if (pending_) {
            snap_ = std::move(pending_);
            pending_.reset();
        }
        hasPending_.store(false, std::memory_order_relaxed);
    }
    if (!snap_) return false;
    const std::uint32_t gen = generation_.load(std::memory_order_acquire);
    if (gen != cacheGeneration_) {
        byRaw_.clear();
        byName_.clear();
        byMaterial_.clear();
        byShader_.clear();
        cacheGeneration_ = gen;
    }
    return true;
}

MaterialPack::Hit MaterialPack::finish(Hit h, std::string_view key) {
    if (h.word == 0) return {};
    if (h.texture != nullptr) {
        const std::uint32_t full = h.texture->bindlessSlot();
        if (!slotFitsWord(full)) {  // D3D12's heap is 65536 entries; the word keeps 14 bits: no entry rather than another texture
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) spdlog::warn("enh: materials: pack map '{}' sits in heap slot {} (> {}): the pack word cannot address it, entry ignored", key, full, kWordSlotMask);
            return {};
        }
        const std::uint32_t slot = full;
        if (slot == 0) return {};
        h.word = (h.word & ~kWordSlotMask) | slot;
    }
    if (h.derived) {
        ++counters_.derived;
        return h;
    }
    ++counters_.matched;
    if (!key.empty()) matched_.emplace(std::string(key), true);
    return h;
}

MaterialPack::Hit MaterialPack::lookup(std::string_view diffuseName) {
    if (!adopt()) return {};
    ++counters_.lookups;
    // the raw name seen before: no key normalisation, no string allocation (its key was noted at the first lookup)
    if (const auto raw = byRaw_.find(diffuseName); raw != byRaw_.end()) return finish(raw->second, {});
    const std::string key = normalizeKey(diffuseName);
    auto it = byName_.find(key);
    if (it == byName_.end()) it = byName_.emplace(key, resolve(*snap_, key)).first;
    byRaw_.emplace(std::string(diffuseName), it->second);
    return finish(it->second, key);
}

MaterialPack::Hit MaterialPack::lookupMaterial(const void* material) {
    if (!adopt()) return {};
    if (material == nullptr) {
        // a draw without a material tag (skinned actors): no pack entry is possible, but the executing shader still tells the engine's specular data
        if (!derive_.enabled) return {};
        namespace mats = engine::materials;
        mats::ShaderInfo di;
        if (!mats::active(di) || (di.cls != mats::ShaderClass::Default && di.cls != mats::ShaderClass::Leafs)) return {};
        ++counters_.lookups;
        if (const auto it = byShader_.find(di.shader); it != byShader_.end()) return finish(it->second, {});
        Hit h;
        if (const std::uint32_t w = deriveWord(specularInfo(di)); w != 0) {
            h.word = w;
            h.derived = true;
        }
        byShader_[di.shader] = h;
        return finish(h, {});
    }
    if (const auto it = byMaterial_.find(material); it != byMaterial_.end()) {
        ++counters_.lookups;
        return finish(it->second, {});
    }
    namespace mats = engine::materials;
    Hit h;
    std::string key;
    const auto byKey = [&](const std::string& k) {
        if (k.empty()) return;
        auto it = byName_.find(k);
        if (it == byName_.end()) it = byName_.emplace(k, resolve(*snap_, k)).first;
        if (it->second.word != 0) {
            h = it->second;
            key = k;
        }
    };
    // the material's own name first (variants of one texture: "..._Glass", "..._Metal"), then its diffuse image
    byKey(normalizeKey(mats::resourceName(material)));
    if (h.word == 0) {
        mats::ShaderInfo info;
        if (mats::active(info, material) && (info.cls == mats::ShaderClass::Default || info.cls == mats::ShaderClass::Leafs) &&
            info.diffuse.source == mats::Source::Sampler && info.diffuse.slot >= 0) {
            byKey(normalizeKey(mats::poolImageName(info, info.diffuse.slot)));
        }
    }
    bool derivedNow = false;
    if (h.word == 0 && derive_.enabled) {
        mats::ShaderInfo di;
        if (mats::active(di, material) && (di.cls == mats::ShaderClass::Default || di.cls == mats::ShaderClass::Leafs)) {
            const SpecularInfo si = specularInfo(di);
            const std::uint32_t w = deriveWord(si);
            if (w != 0) {
                h.word = w;
                h.texture = nullptr;
                h.derived = derivedNow = true;
                spdlog::debug("enh: materials: derived '{}': specular {}{:.2f}, power {} -> roughness {:.2f}", mats::resourceName(material), si.maskMap ? "map " : "const ",
                              si.maskLum, si.powerKnown ? std::format("{:.3f}", si.power) : std::string("texture"), decodeWord(w).roughnessBias);
            }
        }
    }
    byMaterial_[material] = h;
    if (byMaterial_.size() <= 4000) {  // authors: every material seen once, with the names a pack entry can use (-RvkLogLevel debug)
        mats::ShaderInfo si;
        std::string image;
        if (mats::active(si, material) && si.diffuse.source == mats::Source::Sampler && si.diffuse.slot >= 0) image = mats::poolImageName(si, si.diffuse.slot);
        spdlog::debug("enh: materials: seen '{}' (diffuse image '{}'): {}{}", mats::resourceName(material), image,
                      derivedNow ? "derived from the specular data" : h.word != 0 ? "pack entry" : "no entry",
                      h.word != 0 ? std::string() : std::format(" [class {} diffuse {} specular {} power {} enableSpecular {}, specular rgb {:.2f} {:.2f} {:.2f}{}, power {:.3f}{}]",
                                                                 mats::name(si.cls), mats::name(si.diffuse.source), mats::name(si.specular.source),
                                                                 mats::name(si.specularPower.source), si.enableSpecular, si.specular.constant[0], si.specular.constant[1],
                                                                 si.specular.constant[2], si.specular.hasConstant ? "" : " (not read)", si.specularPower.constant[0],
                                                                 si.specularPower.hasConstant ? "" : " (not read)"));
    }
    ++counters_.lookups;
    return finish(h, key);
}

}  // namespace rvk::enh::matpack
