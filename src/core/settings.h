#pragma once

// Player settings: one registry of typed settings that drives bin\rvk\settings.toml, the in-game overlay (src/overlay), the
// A/B comparison and the tool API (rvk_setting_* in include/rvk_api.h). docs/research/overlay.md 2 has the design.
//
// A subsystem registers its settings once at startup and reads them where it needs them:
//
//     auto& water = reg.add({.key = "water.enhanced", .label = "Enhanced water", .category = "Water",
//                            .description = "...", .type = settings::Type::Bool, .defaultValue = 0});
//     reg.onChange(water, [](const settings::Setting& s) { /* render thread, next frame boundary */ });
//     if (water.asBool()) { ... }   // lock-free read, any thread
//
// A setting that needs an optional device capability declares it (docs/research/raytracing.md 3):
//
//     auto& refl = reg.add({.key = "rt.reflections", ..., .requirements = caps::bit(caps::Cap::RayQuery)});
//     if (refl.asBool()) { ... }   // the effective value: false (the parity fallback) while ray query is unavailable
//
// The overlay greys it with the reason ("Ray query: disabled by the setting rt.allow"); the player's value() is kept.
//
// Values change from any thread (overlay, rvk_setting_set, file load); the change callbacks run on the render thread at the
// next frame boundary (Registry::dispatch, called by the HAL before each present). Settings with Flag::Restart only take effect
// at the next start (the overlay marks them while their value differs from the one the session started with).
//
// Two files, two audiences (docs/research/overlay.md 2.3): bin\rvk\settings.toml is the player's file (written by the
// overlay; only values that differ from the defaults are stored), bin\rvk\config.toml stays the developer/debug file. Where
// config.toml sets one of these values explicitly (a non-default value, e.g. run.ps1 -MaxFps), it wins: the setting is
// *locked* for the session (shown read-only, not changed by A/B, the player's own value is kept in settings.toml).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/caps.h"

namespace rvk::settings {

enum class Type : std::uint8_t { Bool, Int, Float, Enum };

namespace Flag {
inline constexpr std::uint32_t Restart = 1u << 0;  // takes effect at the next start
inline constexpr std::uint32_t NoAB = 1u << 1;     // not part of the A/B comparison (hotkeys, overlay options)
inline constexpr std::uint32_t Hidden = 1u << 2;   // not shown in the overlay (file / API only)
// developer setting (debug views, A/B): shown in the overlay only in debug mode (Registry::setDebugSwitch); outside it the
// setting applies as its default, the player's value is kept
inline constexpr std::uint32_t Debug = 1u << 3;
// fine tuning: shown in the overlay only with "Show advanced settings" (overlay.show_advanced) or in debug mode
inline constexpr std::uint32_t Advanced = 1u << 4;
}  // namespace Flag

// overlay tabs (Spec::tab)
inline constexpr const char* kTabDisplay = "Display";
inline constexpr const char* kTabGraphics = "Graphics";
inline constexpr const char* kTabSystem = "System";
inline constexpr const char* kTabDebug = "Debug";  // shown in debug mode only

struct Spec {
    std::string key;          // "<table>.<name>" (lower case, digits, '_'): stable, it is the TOML path and the API key
    std::string label;        // overlay label
    std::string category;     // overlay section (inside the tab)
    std::string tab = kTabGraphics;  // overlay tab: kTabDisplay / kTabGraphics / kTabSystem / kTabDebug
    std::string description;  // overlay tooltip (player language; developer details only for Flag::Debug settings)
    Type type = Type::Bool;
    double defaultValue = 0.0;  // Bool 0/1, Enum: option index
    double min = 0.0;           // Int / Float range (values are clamped)
    double max = 1.0;
    std::vector<std::string> options;       // Enum: the values as written to the file / API, e.g. {"engine", "on", "off"}
    std::vector<std::string> optionLabels;  // Enum: overlay labels (optional, same order)
    std::string format;                     // Int / Float overlay format, e.g. "%.0f fps" (optional)
    std::string autoLabel;                  // Int / Float: the overlay shows this instead of the minimum ("Unlimited", "Auto")
    std::uint32_t flags = 0;                // Flag::*
    // Capabilities the setting needs (caps::bit(caps::Cap::RayQuery) | ...; docs/research/raytracing.md 3). While one is
    // unmet (no such hardware, disabled by rt.allow, no Vulkan device yet) the overlay shows the setting greyed with the
    // reason, and its *effective* value (asBool / asInt / asFloat / asEnum) is `unmetValue` (default: defaultValue = the
    // parity path); the player's value itself is kept (file, API) and applies again when the capability is there.
    caps::Mask requirements = 0;
    std::optional<double> unmetValue;
};

class Registry;

class Setting {
public:
    const Spec& spec() const { return spec_; }
    const std::string& key() const { return spec_.key; }
    Type type() const { return spec_.type; }
    bool restart() const { return (spec_.flags & Flag::Restart) != 0; }
    bool abEligible() const { return (spec_.flags & (Flag::NoAB | Flag::Restart)) == 0 && !locked(); }

    // the player's (requested) value: what the file, the API and A/B store (any thread, lock-free)
    double value() const { return value_.load(std::memory_order_relaxed); }
    // capability resolution (Spec::requirements): false while a required capability is unmet
    bool available() const { return !unmet_.load(std::memory_order_relaxed); }
    // Flag::Debug outside debug mode: applies as the default
    bool gated() const { return gated_.load(std::memory_order_relaxed); }
    // the value that applies: value(), the spec's unmetValue while unavailable, the default while gated. Features read these:
    double effective() const { return !available() ? fallback_ : gated() ? spec_.defaultValue : value(); }
    bool asBool() const { return effective() != 0.0; }
    int asInt() const { return static_cast<int>(effective()); }
    float asFloat() const { return static_cast<float>(effective()); }
    // Enum: the option name of the effective value
    std::string_view asEnum() const;
    bool isDefault() const { return value() == spec_.defaultValue; }

    // developer override from config.toml (fixed for the session)
    bool locked() const { return locked_.load(std::memory_order_relaxed); }
    // the value the session started with (Restart settings run with it)
    double startValue() const { return start_; }
    bool restartPending() const { return restart() && value() != start_; }

private:
    friend class Registry;
    explicit Setting(Spec spec) : spec_(std::move(spec)) {}
    Spec spec_;
    std::atomic<double> value_{0.0};
    std::atomic<bool> locked_{false};
    std::atomic<bool> unmet_{false};
    std::atomic<bool> gated_{false};
    double fallback_ = 0.0;  // effective value while unmet (Spec::unmetValue or the default)
    std::string unmetReason_;
    double start_ = 0.0;
    double stored_ = 0.0;  // locked: the player's own value (written to settings.toml instead of the forced one)
    std::string lockReason_;
    double other_ = 0.0;   // the inactive side of the A/B comparison
    bool pending_ = false;  // changed since the last dispatch
    struct Callback {
        const void* owner = nullptr;
        std::function<void(const Setting&)> fn;
    };
    std::vector<Callback> callbacks_;
};

enum class Result : std::int32_t {
    Ok = 0,
    UnknownKey = -1,
    BadValue = -2,
    Locked = -3,
};
std::string_view toString(Result r);

class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Registers a setting (the value starts at the default). The reference stays valid for the registry's lifetime. A
    // duplicate or malformed key returns the existing / a hidden dummy setting and logs an error (never throws).
    Setting& add(Spec spec);
    Setting* find(std::string_view key);
    const Setting* find(std::string_view key) const;
    // registration order
    std::vector<Setting*> all() const;
    std::size_t size() const;

    // ---- values (any thread) ----
    // clamps Int / Float to the range (Int rounded), Enum / Bool must be valid; a change marks the setting for dispatch and
    // the file as dirty
    Result set(Setting& s, double value);
    Result set(std::string_view key, double value);
    // text form (API, file): Bool "true"/"false"/"on"/"off"/"1"/"0", numbers, Enum option names (case-insensitive) or index
    Result setText(std::string_view key, std::string_view text);
    static bool parseText(const Spec& spec, std::string_view text, double& out);
    // the value as text: "true", "60", "0.5", "off"
    static std::string formatValue(const Spec& spec, double value);
    std::string text(const Setting& s) const { return formatValue(s.spec(), s.value()); }
    void resetToDefault(Setting& s) { set(s, s.spec().defaultValue); }

    // developer override (config.toml): forces `value` for the session; the player's value is kept for the file
    void lock(Setting& s, double value, std::string reason);
    std::string lockReason(const Setting& s) const;

    // ---- capability resolution (Spec::requirements, core/caps.h) ----
    // The renderer calls this once its device exists (and again if it changes). Until then every setting with requirements is
    // unavailable ("no Vulkan device"). Settings whose effective value changes are dispatched like a change.
    void setCapabilities(const caps::Report& report);
    caps::Report capabilities() const;
    // why `s` is unavailable ("" when available)
    std::string unavailableReason(const Setting& s) const;
    // the requirements of a spec against a report: "" when met (pure; also what setCapabilities applies)
    static std::string resolve(const Spec& spec, const caps::Report& report);

    // ---- debug mode ----
    // `s` (a Bool) switches debug mode: while it is false, Flag::Debug settings are gated (effective = default, hidden in the
    // overlay) and switching it off returns the A/B comparison to side A. Without a switch every setting applies (debug mode).
    void setDebugSwitch(Setting& s);
    bool debugMode() const;

    // ---- change callbacks ----
    // `fn` runs on the render thread (dispatch) after each change of `s`, also after A/B flips and file loads. `owner`
    // identifies the subscriber for removeCallbacks (an object that goes away before the registry must remove its callbacks).
    void onChange(Setting& s, std::function<void(const Setting&)> fn, const void* owner = nullptr);
    void removeCallbacks(const void* owner);
    // runs the callbacks of the settings changed since the last call; returns how many settings changed
    std::size_t dispatch();
    // incremented on every change (the overlay redraws its diff list)
    std::uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

    // ---- A/B comparison ----
    // Two sides: the active values and a stored other side (initially the defaults). flipAB swaps
    // them for every A/B-eligible setting (not NoAB, not Restart, not locked). Side A is the one the session starts with.
    bool bActive() const { return bActive_.load(std::memory_order_relaxed); }
    void flipAB();
    void setSide(bool b) {
        if (b != bActive()) flipAB();
    }
    // copy the active side's values to the other side
    void copyActiveToOther();
    // reset the other side to the defaults
    void resetOther();
    // the other side's value of a setting
    double otherValue(const Setting& s) const;
    // eligible settings whose two sides differ
    std::vector<const Setting*> abDifferences() const;

    // ---- persistence ----
    // TOML text of settings.toml: side A's non-default values in their tables, side B's non-default values under [ab.*];
    // keys sorted by registration order. Locked settings write the player's value.
    std::string toToml() const;
    // applies a settings.toml text: known keys are set (locked ones keep the override, the value is remembered as the player's),
    // unknown keys / bad values are skipped with a warning in `warnings`; false on a TOML syntax error (`warnings` has it)
    bool fromToml(std::string_view text, std::vector<std::string>* warnings = nullptr);
    // a missing file is not an error (defaults)
    bool load(const std::filesystem::path& file, std::vector<std::string>* warnings = nullptr);
    // writes "<file>.tmp", then renames it over `file`
    bool save(const std::filesystem::path& file, std::string* error = nullptr);
    bool dirty() const { return dirty_.load(std::memory_order_relaxed); }
    void clearDirty() { dirty_ = false; }
    // the start values of Restart settings (call after loading the files)
    void markStarted();

private:
    Result setLocked(Setting& s, double value);  // mutex_ held
    void changedLocked(Setting& s);              // mutex_ held

    void resolveLocked(Setting& s);              // mutex_ held
    void gateLocked();                           // mutex_ held: Flag::Debug settings follow the debug switch
    void flipLocked();                           // mutex_ held

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Setting>> settings_;
    Setting* debugSwitch_ = nullptr;
    caps::Report caps_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> dirty_{false};
    std::atomic<bool> bActive_{false};
};

// the process-wide registry (bootstrap registers the renderer's settings and loads settings.toml at attach)
Registry& registry();

// ---- the renderer's own settings (registered by registerRendererSettings) ----
struct RendererSettings {
    Setting* vsync = nullptr;        // vulkan.vsync: "engine" (the game's option), "on", "off"
    Setting* maxFps = nullptr;       // vulkan.max_fps: 0 = no cap beyond vsync
    Setting* menuFps = nullptr;      // vulkan.menu_fps: cap while a menu page is open (engine/menu_state.h); 0 = none
    Setting* exclusiveAcquire = nullptr;  // vulkan.exclusive_acquire: hold Vulkan's exclusive mode in exclusive fullscreen (hidden diagnostics)
    Setting* fullscreen = nullptr;   // vulkan.fullscreen: "borderless", "exclusive" (needs FullScreenExclusive; window-mode.md 8)
    Setting* adapter = nullptr;      // vulkan.adapter: -1 = best discrete GPU (restart)
    Setting* toggleKey = nullptr;    // overlay.toggle_key
    Setting* abKey = nullptr;        // overlay.ab_key
    Setting* fpsCounter = nullptr;   // overlay.fps_counter: small fps readout while the overlay is closed
    Setting* abBadge = nullptr;      // overlay.ab_badge: "B" tag while side B is active and the overlay is closed
    Setting* debugMode = nullptr;    // overlay.debug_mode: shows the Flag::Debug settings, the A/B comparison, diagnostics
    Setting* showAdvanced = nullptr; // overlay.show_advanced: shows the Flag::Advanced settings (hidden: the overlay's checkbox)
    Setting* tintDraws = nullptr;    // debug.tint_draws: "off", "kind", "pass" (d3d9/draws.h TintMode)
    Setting* drawRecords = nullptr;  // debug.draw_records: per-frame draw records (hidden; the histogram is always on)
    Setting* paceLoading = nullptr;  // debug.pace_loading: loading screens wait for the pacer (hidden; load-time A/B)
    Setting* batchDraws = nullptr;   // debug.batch_draws: the opaque passes' draws grouped by pipeline (d3d9::Device::setBatching)
    Setting* injectView = nullptr;   // debug.inject_view: "off", "depth", "tint" (enh/debug_view.h, an injected pass)
    Setting* rtAllow = nullptr;      // rt.allow: enable the ray tracing extensions at device creation (restart)
    Setting* rtReflections = nullptr; // rt.reflections: "off", "low", "medium", "high" (the water's ray-traced reflections, needs RayQuery)
    Setting* rtDebug = nullptr;      // rt.debug: "off", "instances", "depth", "normals" (enh/rt_debug.h, needs RayQuery)
    Setting* rtReflectionTweaks = nullptr;  // rt.reflection_tweaks: hidden diagnostics bitmask of the water's RT reflections (enh/water.h)
    Setting* rtSceneViews = nullptr; // rt.scene_views: "all", "main", "shadow": which views feed the RT scene (hidden, diagnostics)
    // enhanced water (enh/water.h, docs/research/water.md)
    Setting* waterEnabled = nullptr;      // water.enabled
    Setting* waterQuality = nullptr;      // water.quality: "low", "medium", "high"
    Setting* waterReflections = nullptr;  // water.reflections: "sky" (cube only), "ssr"
    Setting* waterOcean = nullptr;        // water.ocean: "off", "gerstner", "fft" (the open sea's wave grid + lower coast)
    Setting* waterOceanQuality = nullptr; // water.ocean_quality: "low", "medium", "high" (FFT tiles)
    Setting* waterSeaState = nullptr;     // water.sea_state: "auto" (weather), "calm", "moderate", "rough", "storm"
    Setting* waterWindDirection = nullptr;  // water.wind_direction: degrees, -1 = auto
    Setting* waterWaveScale = nullptr;    // water.wave_scale: multiplier
    Setting* waterMurkiness = nullptr;    // water.murkiness: multiplier on the absorption
    Setting* waterReflectionStrength = nullptr;  // water.reflection_strength
    Setting* waterDebug = nullptr;        // water.debug: "off", "shore_field", "depth", "normals", "reflection"
    // enhanced sun shadows (enh/shadows.h, docs/research/shadows.md)
    Setting* shadowsEnabled = nullptr;    // shadows.enabled
    Setting* shadowsFilter = nullptr;     // shadows.filter: "low", "medium", "high" (bilinear taps per axis 2 / 3 / 4)
    Setting* shadowsSoftness = nullptr;   // shadows.softness: tap spacing in texels
    Setting* shadowsBlend = nullptr;      // shadows.cascade_blend: fraction of a cascade blended into the next
    Setting* shadowsFade = nullptr;       // shadows.fade: fraction of the last cascade over which shadows fade out
    Setting* shadowsBias = nullptr;       // shadows.bias: constant depth bias (texels of a 45° slope)
    Setting* shadowsDebug = nullptr;      // shadows.debug: "off", "cascades"
    Setting* shadowsMapSize = nullptr;    // shadows.map_size: "auto" (4096 with enhanced shadows, else the game's), "game", "1024", "2048", "4096"
    Setting* shadowsRange = nullptr;      // shadows.range: metres, 0 = auto (100 with enhanced shadows, else the game's)
    Setting* shadowsSplit = nullptr;      // shadows.split: 0 (logarithmic) .. 1 (uniform) cascade splits with an overridden range
    // ray-traced global illumination (enh/gi.h, docs/research/rtgi.md; needs RayQuery)
    Setting* giEnabled = nullptr;         // gi.enabled
    Setting* giQuality = nullptr;         // gi.quality: "low", "medium", "high" (probes traced per frame)
    Setting* giStrength = nullptr;        // gi.strength: 0 = the engine's ambient .. 1 = GI
    Setting* giLightmaps = nullptr;       // gi.lightmaps: "keep", "replace"
    Setting* giCharacters = nullptr;      // gi.characters: skinned draws in the RT scene (enh/rt_scene)
    Setting* giScreen = nullptr;          // gi.screen: "off", "low", "medium", "high" (stage C screen probes, needs the G-buffer)
    Setting* giDebug = nullptr;           // gi.debug: "off", "irradiance", "cascades"
    // G-buffer / motion vectors / GTAO (enh/gbuffer.h, docs/research/rtgi.md 10)
    Setting* aoEnabled = nullptr;         // ao.enabled: GTAO on the ambient light
    Setting* aoMode = nullptr;            // ao.mode: "replace" (the game's SSAO off), "add"
    Setting* aoStrength = nullptr;        // ao.strength: 0..1
    Setting* aoRadius = nullptr;          // ao.radius: metres
    Setting* gbufferDebug = nullptr;      // gbuffer.debug: "off", "normals", "motion", "ao"
    // detail distances (engine/lod.h, docs/research/lod.md): multipliers, 1 = the game's
    Setting* lodTrees = nullptr;          // lod.trees: SpeedTree LOD + billboard fade distances
    Setting* lodMeshes = nullptr;         // lod.meshes: mesh LOD ranges (eCMesh_PS)
    Setting* lodView = nullptr;           // lod.view: the view range of objects (eCFrustumDatabase view range factor)
    Setting* highFps = nullptr;           // compat.high_fps: the high frame rate gameplay fixes (engine/high_fps.h)
};
// hotkey names of overlay.toggle_key / overlay.ab_key ("none" disables)
const std::vector<std::string>& hotkeyNames();
// registers them in `reg`; for the process-wide registry() they are also what renderer() returns
RendererSettings registerRendererSettings(Registry& reg);
// the renderer's settings in registry() (null pointers before registerRendererSettings(registry()))
const RendererSettings& renderer();

// ---- graphics presets (the overlay's Graphics tab) ----
struct Preset {
    std::string name;   // "original", "low", ...
    std::string label;  // overlay
    std::vector<std::pair<std::string, std::string>> values;  // key -> value text (Registry::setText)
};
// Original (the game's look), Low, Medium, High (= the defaults), Ultra
const std::vector<Preset>& graphicsPresets();
// sets the preset's values (locked settings keep theirs)
void applyPreset(Registry& reg, const Preset& preset);
// index of the first preset whose values all match the current ones, -1 = custom
int matchPreset(const Registry& reg);
// resets every setting of an overlay tab to its default (locked ones stay); returns how many changed
std::size_t resetTab(Registry& reg, std::string_view tab);

}  // namespace rvk::settings
