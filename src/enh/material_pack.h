#pragma once

// Material pack (docs/research/material-pack.md): extra PBR data for the game's materials as a separate texture pack, keyed by
// the NAME of a material's diffuse image (the engine's pool image title, e.g. "Arch_House_Wall_01_Diffuse_01", engine/materials.h).
//
//   <pack dir>/<DiffuseName>_orm.dds|png   R occlusion, G roughness, B metalness, A emissive mask (optional)
//   <pack dir>/<DiffuseName>_r.dds|png     roughness in R (single channel)
//   <pack dir>/pack.toml                   optional per material / wildcard constants (see parseManifest)
//
// The maps use the diffuse texture's uv (any resolution). Pure logic here (names, manifest, the 32-bit word the shaders decode);
// the loader (MaterialPack) reads the files on a worker thread into d3d9::Textures (bindless slots) and answers lookups on the
// render thread. A "pack word" = bindless slot (bits 0-13, 0 = no map) | flags/constants (bits 14-31), mirrored in
// shaders/include/rvk_material_variant.hlsli (rvkPackDecode) and rvk_rt.hlsli.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rvk::d3d9 {
class Device;
class Texture;
}  // namespace rvk::d3d9
namespace rvk::engine::materials {
struct ShaderInfo;
}

namespace rvk::enh::matpack {

// ---- names ---------------------------------------------------------------------------------------------------------------
// the key of a texture / material name: base name without directories and image extensions (.dds .png ._ximg .ximg), lower case
std::string normalizeKey(std::string_view name);

enum class Kind : std::uint8_t { Unknown, Orm, Roughness };
struct FileName {
    std::string key;  // normalised diffuse name the file belongs to
    Kind kind = Kind::Unknown;
};
// "Wood_01_Diffuse_01_orm.dds" -> {"wood_01_diffuse_01", Orm}; "_r" / "_rough" / "_roughness" = Roughness; other = Unknown (key = whole name)
FileName classifyFile(std::string_view fileName);

// case-insensitive wildcard match ('*' any run, '?' one character) of a normalised key
bool globMatch(std::string_view pattern, std::string_view text);

// ---- constants / manifest ----------------------------------------------------------------------------------------------------
struct Constants {
    std::optional<float> roughnessScale;  // rough = saturate(map * scale + bias); default 1
    std::optional<float> roughnessBias;   // default 0
    std::optional<float> roughness;       // constant roughness (no map / the map is ignored when given without scale / bias)
    std::optional<float> metalness;       // ORM: multiplies the map's B (default 1); no ORM: the constant (default 0)
    std::optional<float> emissive;        // > 0 with an ORM map: its A channel marks emissive texels (diffuse colour added)
    // carried for later consumers (glass / wet surfaces / path tracing); parsed and logged, not used by the raster shader yet
    std::optional<float> ior, transmission, wetness;
    std::optional<std::string> map;       // explicit map file (relative to the manifest), instead of the naming convention
    void overlay(const Constants& o);     // fields set in `o` replace this one's
};
struct Rule {
    std::string pattern;  // normalised key or wildcard
    Constants c;
};
struct Manifest {
    Constants defaults;       // [defaults]: applied to every entry (not to materials without any entry)
    std::vector<Rule> rules;  // [material."name"] in file order; later rules override earlier ones
    std::vector<std::string> warnings;
};
// TOML:
//   [defaults]                       roughness_scale = 1.0
//   [material."Wood_*_Diffuse_01"]   roughness = 0.6   metalness = 0.0   map = "wood.dds"   emissive = 1.0
std::optional<Manifest> parseManifest(std::string_view text, std::string* error);
// defaults + every matching rule in order
Constants resolveConstants(const Manifest& m, std::string_view key);
bool hasRule(const Manifest& m, std::string_view key);  // an exact / wildcard rule exists for the key

// ---- the pack word ---------------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kWordSlotMask = 0x3FFFu;  // bindless 2D slots are < 16384 (vk::Bindless::kMax2D)
inline constexpr std::uint32_t kWordPresent = 1u << 14;
inline constexpr std::uint32_t kWordOrm = 1u << 15;       // layout ORM (else roughness in R)
inline constexpr std::uint32_t kWordGlass = 1u << 30;     // glass: full reflection (no light cap), the engine's diffuse stays
inline constexpr std::uint32_t kWordEmissive = 1u << 31;  // the map's A channel is an emissive mask
// DERIVED words (a draw WITHOUT a pack entry whose engine material has specular data, docs/research/material-pack.md 6): slot 0 and bit 15 set (an ORM layout needs a
// map, so the combination is free), bits 16-23 = the roughness (8 bit, /255), metalness / wetness / glass / emissive = 0. Consumers decode them like a constant entry
// with roughness scale 0 and bias = the roughness; only the raster variant treats them differently (the engine's own Phong highlights stay: no direct GGX added).
inline constexpr std::uint32_t kWordDerived = 1u << 15;
struct Decoded {
    std::uint32_t slot = 0;
    bool present = false, orm = false, emissive = false;
    bool glass = false, derived = false;
    float roughnessScale = 1.0f, roughnessBias = 0.0f, metalness = 0.0f, wetness = 0.0f;
};
// roughness = saturate(tex * scale + bias): scale = bits 16-19 / 8 (0..1.875), bias = bits 20-23 / 15, metalness (scale / constant) = bits 24-27 / 15,
// wetness = bits 28-29 / 3, glass = bit 30, emissive = bit 31
std::uint32_t encodeWord(std::uint32_t slot, bool orm, const Constants& c, float defaultRoughness);
Decoded decodeWord(std::uint32_t word);
// ---- roughness derived from the engine's own specular data (materials.derive_from_specular) ---------------------------------------------------------------
// What the engine's material graph says about gloss: the Specular role (a colour mask: a map or a constant colour) and the SpecularPower role (a constant, a map or
// absent = the engine's default 1.0); the engine's Phong exponent is n = SpecularPower * 128 (ge_default.fxh PhongSpecular).
struct SpecularInfo {
    bool hasMask = false;      // the material has a Specular role
    bool maskMap = false;      // ... a texture (its mean is unknown on the CPU)
    float maskLum = 1.0f;      // a constant mask's luminance
    bool powerKnown = false;   // SpecularPower is a constant (or absent: the engine's default 1.0)
    float power = 1.0f;        // that value (the engine multiplies it by 128)
    bool operator==(const SpecularInfo&) const = default;
};
struct DeriveOptions {
    bool enabled = false;
    float minRoughness = 0.25f, maxRoughness = 0.9f;  // the derived roughness is clamped to [min, max] (before the strength folds in)
};
struct Derived {
    bool valid = false;        // the material has specular data
    float roughness = 1.0f;    // effective roughness: the exponent's roughness (clamped), pushed towards 1 by a weak specular mask
    float strength = 0.0f;     // the specular mask's strength 0..1
};
// Blinn / Phong exponent -> GGX: alpha = sqrt(2 / (n + 2)) (the usual Phong-to-GGX match), the pack's roughness r has alpha = r * r (shaders: a = max(r * r, 0.0025)), so r = sqrt(alpha)
float roughnessFromPower(float specularPower);
inline constexpr float kMapMaskStrength = 0.8f;  // a Specular MAP's strength (the per-pixel mask is not read: skin / cloth maps are mostly dark with bright spots)
Derived deriveFromSpecular(const SpecularInfo& s, const DeriveOptions& o, float defaultRoughness);
std::uint32_t encodeDerivedWord(float roughness);
// the engine's material graph -> SpecularInfo (engine/materials.h roles; the Specular / SpecularPower proxies' constants and swizzle)
SpecularInfo specularInfo(const engine::materials::ShaderInfo& info);

// the roughness / metalness the shader computes for a texel (R, G, B, A of the map), for tests and the CPU mirror
struct Surface {
    float occlusion = 1.0f, roughness = 1.0f, metalness = 0.0f, emissive = 0.0f;
};
Surface evaluate(const Decoded& d, const float texel[4]);

// hot reload (command "materials reload", overlay / -Commands): the HAL installs the hook that re-reads the pack folders; false = no pack loaded
void setReloadHook(std::function<bool()> hook);
bool requestReload();

// ---- the loader --------------------------------------------------------------------------------------------------------------------
struct Stats {
    std::uint32_t files = 0, entries = 0, loaded = 0, failed = 0, rules = 0;
    std::uint64_t bytes = 0;
    double ms = 0.0;
};

class MaterialPack {
public:
    explicit MaterialPack(d3d9::Device& device);
    ~MaterialPack();
    MaterialPack(const MaterialPack&) = delete;
    MaterialPack& operator=(const MaterialPack&) = delete;

    // roots: the folders to read (later ones override earlier), budgetBytes: decoded texture memory limit. Starts a (re)load on a
    // worker thread (hot reload = call again); an empty list unloads.
    void configure(bool enabled, std::vector<std::filesystem::path> roots, std::uint64_t budgetBytes, float defaultRoughness);

    struct Hit {
        std::uint32_t word = 0;          // 0 = no entry
        d3d9::Texture* texture = nullptr;  // the map (null: constants only)
        bool derived = false;              // no pack entry: the word is derived from the engine's specular data
    };
    // The entry of a diffuse image name (any case / extension; render thread). Cached per (name, generation).
    Hit lookup(std::string_view diffuseName);
    // the entry of a draw's material (the engine's material graph -> its diffuse pool image -> lookup); cached per material pointer
    Hit lookupMaterial(const void* material);

    bool enabled() const { return enabled_; }
    std::uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    Stats stats() const;
    // counters since the last call (matched = lookups with an entry), for the 5 s log line
    struct Counters {
        std::uint64_t lookups = 0, matched = 0, derived = 0;
    };
    Counters takeCounters() {
        Counters c = counters_;
        counters_ = {};
        return c;
    }
    // materials.derive_from_specular (render thread): applies to the lookups after the call (the generation changes: caches re-look-up)
    void setDerive(const DeriveOptions& o);
    const DeriveOptions& deriveOptions() const { return derive_; }
    // the derived word of a material's specular data (0 = none / derive off), for consumers that do not hold the engine material (the RT scene)
    std::uint32_t deriveWord(const SpecularInfo& s) const;
    // names of the entries that matched a lookup (diagnostics, first N)
    std::size_t matchedNames() const { return matched_.size(); }

private:
    struct Entry {
        mutable std::unique_ptr<d3d9::Texture> texture;  // created at the first lookup that matches (render thread): unused maps cost nothing
        mutable bool tried = false;
        Constants c;
        bool orm = false;
        std::string file;
    };
    struct Snapshot {
        std::unordered_map<std::string, Entry> entries;  // by key
        Manifest manifest;                               // wildcard rules (constants only)
        Stats stats;
        float defaultRoughness = 0.7f;
        std::uint64_t budget = 0;
        mutable std::uint64_t used = 0;  // decoded bytes of the maps created so far
    };
    // decodes the entry's file into a texture (render thread, at its first match); null = failed / over the budget (logged)
    std::unique_ptr<d3d9::Texture> createMap(const Entry& e, const Snapshot& s, std::uint64_t& bytes);
    void load(std::vector<std::filesystem::path> roots, std::uint64_t budget, float defaultRoughness, std::uint32_t token);
    Hit resolve(const Snapshot& s, std::string_view key);
    bool adopt();
    Hit finish(Hit h, std::string_view key);

    d3d9::Device& dev_;
    bool enabled_ = false;
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<std::uint32_t> loadToken_{0};
    std::shared_ptr<const Snapshot> snap_;  // render thread; replaced from `pending_` at a lookup
    std::mutex mutex_;                      // pending_
    std::shared_ptr<const Snapshot> pending_;
    std::thread worker_;
    std::unordered_map<std::string, Hit> byName_;           // lookup cache of this generation
    std::unordered_map<const void*, Hit> byShader_;         // shader pointer -> derived hit (draws without a material tag)
    std::unordered_map<const void*, Hit> byMaterial_;       // material pointer -> hit (cleared on a new generation)
    std::unordered_map<std::string, bool> matched_;         // keys that matched (log)
    Counters counters_;
    DeriveOptions derive_;
    std::uint32_t cacheGeneration_ = 0;
};

}  // namespace rvk::enh::matpack
