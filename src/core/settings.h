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

// A prerequisite of a setting (Spec::needs): the setting acts only while `key`'s effective value equals `value` (text form,
// Registry::parseText; `negate`: while it does NOT equal it, e.g. {"shadows.rt", "off", true} = ray-traced shadows on).
struct Need {
    std::string key;
    std::string value;
    bool negate = false;
    // false: never switched on for the player (the renderer mode: Wrapper / Full needs a restart and is the player's own choice);
    // an unmet need of this kind only greys the dependent with the reason
    bool autoApply = true;
};

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
    // Enum: the capabilities an option needs (same order as `options`; missing / 0 = none). An option whose requirements are unmet
    // is greyed in the overlay and, when it is the player's value, applies as the next lower option that is available
    // (options are ordered by what they need); the player's choice is kept. E.g. framegen.mode: "fsr4" needs the FSR 4 frame generation provider.
    std::vector<caps::Mask> optionRequirements;
    // Enum without Flag::Restart: a restart group per option (same order as `options`). Switching between options of one
    // group applies at once; between groups only at the next start (restartPending, "restart needed" in the overlay), e.g.
    // renderer.mode: original {0} | wrapper, full {1}. Empty = no such rule.
    std::vector<std::uint8_t> restartGroups;
    // Prerequisites (docs/research/settings-audit.md; Registry::setWithNeeds / needsReason): all must be met for the setting to act.
    // Rules: (1) turning a dependent ON through setWithNeeds (a Bool true, an Enum option > 0 of an option list that starts with
    // "off") switches its unmet prerequisites on first (chains followed, cycles cut; a restart-only prerequisite is set and shows
    // "restart needed" like any restart setting); (2) turning a prerequisite OFF never changes its dependents: they are greyed
    // with the reason (needsReason) and act again when it is back; (3) a prerequisite that cannot be satisfied (locked,
    // capability unmet, `autoApply` false) greys the dependent with that reason. The effective value is never changed by needs.
    std::vector<Need> needs;
    // Overlay placement (the pages are generated from these; docs/research/overlay.md 2.8). Keep them next to the registration:
    // removing a setting removes its place. page = a sidebar page (kPage*, see pageOrder()); section = the heading inside the
    // page (empty = `category`); order = position inside its section / the section's position in the page (lower first,
    // ties keep the registration order). The tooltip is `description`, the tier comes from the flags (tierOf).
    std::string page;
    std::string section;
    int order = 0;
};

// the sidebar pages in order (the Debug page is shown in debug mode only)
inline constexpr const char* kPageDisplay = "Display";
inline constexpr const char* kPageLighting = "Lighting & Shadows";
inline constexpr const char* kPageRayTracing = "Ray Tracing";
inline constexpr const char* kPageWater = "Water & Materials";
inline constexpr const char* kPageImage = "Image";
inline constexpr const char* kPageDetail = "Detail Distances";
inline constexpr const char* kPageSystem = "System";
inline constexpr const char* kPageDebug = "Debug";
// the settings pages in sidebar order (the Performance page of the overlay has no settings)
const std::vector<std::string>& pageOrder();

enum class Tier : std::uint8_t { Basic, Advanced, Debug, Hidden };
Tier tierOf(const Spec& spec);
// the page / section a spec is shown in (explicit metadata, else derived from `tab` / `category`)
std::string pageOf(const Spec& spec);
std::string sectionOf(const Spec& spec);

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
    double effective() const {
        if (!available()) return fallback_;
        if (gated()) return spec_.defaultValue;
        double v = value();
        // an option with unmet requirements applies as the next lower option that is available
        while (v > 0.0 && !optionAvailable(v)) v -= 1.0;
        return v;
    }
    // Enum: whether the option at index `v` has its capability requirements (Spec::optionRequirements)
    bool optionAvailable(double v) const {
        const auto i = static_cast<std::size_t>(v);
        return i >= 32 || ((optionUnmet_.load(std::memory_order_relaxed) >> i) & 1u) == 0;
    }
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
    bool restartPending() const {
        if (restart()) return value() != start_;
        return !spec_.restartGroups.empty() && restartGroup(value()) != restartGroup(start_);
    }

private:
    friend class Registry;
    explicit Setting(Spec spec) : spec_(std::move(spec)) {}
    int restartGroup(double v) const {
        const auto i = static_cast<std::size_t>(v);
        return i < spec_.restartGroups.size() ? spec_.restartGroups[i] : -1;
    }
    Spec spec_;
    std::atomic<double> value_{0.0};
    std::atomic<bool> locked_{false};
    std::atomic<bool> unmet_{false};
    std::atomic<std::uint32_t> optionUnmet_{0};  // bit i: option i's requirements are unmet
    std::vector<std::string> optionReasons_;     // per option: why (empty = met)
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
    // why option `index` of an Enum setting is unavailable ("" when it is available)
    std::string optionUnavailableReason(const Setting& s, std::size_t index) const;
    // the requirements of a spec against a report: "" when met (pure; also what setCapabilities applies)
    static std::string resolve(const Spec& spec, const caps::Report& report);

    // ---- prerequisites (Spec::needs) ----
    struct NeedChange {
        std::string key;
        std::string from;  // text form before
        std::string to;    // text form after
        bool restart = false;  // the change takes effect only at the next start (Flag::Restart / a restart group)
    };
    struct NeedsResult {
        Result result = Result::Ok;           // of the requested change itself
        std::vector<NeedChange> changes;      // prerequisites switched on (the overlay's "Also switched on: ..." toast), nearest first
        std::string blocked;                  // prerequisites that could not be satisfied ("" = none), "; "-separated reasons
    };
    // set(key, text) + switching on the prerequisites of the new value (see Spec::needs); a change that turns the setting off
    // or leaves it off touches nothing else
    NeedsResult setWithNeeds(std::string_view key, std::string_view text);
    // "" when every prerequisite of `s` is met (transitively: a met prerequisite that is itself greyed counts as unmet), else
    // "Needs <label> = <value>" lines joined by "; " (the overlay's greyed reason)
    std::string needsReason(const Setting& s) const;
    // the same by key: "key=value" / "key!=value" of every unmet prerequisite (direct ones only), ", " joined; "" = all met (the stage line)
    std::string unmetNeedKeys(const Setting& s) const;
    // the keys of the settings that list `key` as a prerequisite (the overlay indents them under it)
    std::vector<std::string> dependentsOf(std::string_view key) const;
    // "key=value" / "key!=value" lines of a spec's needs, newline separated (the tool API)
    static std::string needsText(const Spec& spec);

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
    static void resolveOptions(Setting& s, const caps::Report& report);
    bool needMet(const Need& n, const Setting*& prereq) const;
    bool needsOn(const Setting& s, double v) const;
    void applyNeeds(Setting& s, std::vector<Setting*>& visiting, NeedsResult& out);
    std::string needsReasonDepth(const Setting& s, int depth) const;
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
    // renderer.mode: "original", "wrapper", "full" (core/config.h RenderMode; original <-> the others after a restart)
    Setting* rendererMode = nullptr;
    Setting* vsync = nullptr;        // vulkan.vsync: "engine" (the game's option), "on", "off"
    Setting* maxFps = nullptr;       // vulkan.max_fps: 0 = no cap beyond vsync
    Setting* menuFps = nullptr;      // vulkan.menu_fps: cap while a menu page is open (engine/menu_state.h); 0 = none
    Setting* exclusiveAcquire = nullptr;  // vulkan.exclusive_acquire: hold Vulkan's exclusive mode in exclusive fullscreen (hidden diagnostics)
    Setting* fullscreen = nullptr;   // vulkan.fullscreen: "borderless", "exclusive" (needs FullScreenExclusive; window-mode.md 8)
    Setting* api = nullptr;          // vulkan.api: vulkan | d3d12 (restart)
    Setting* adapter = nullptr;      // vulkan.adapter: -1 = best discrete GPU (restart)
    Setting* igpuPresetDone = nullptr; // system.igpu_preset_done (hidden): the integrated-GPU defaults were applied once (applyIntegratedDefaults)
    Setting* lowBandwidth = nullptr; // vulkan.low_bandwidth: auto | off | on (core/low_bandwidth.h; auto = on for integrated GPUs)
    Setting* toggleKey = nullptr;    // overlay.toggle_key
    Setting* abKey = nullptr;        // overlay.ab_key
    Setting* fpsCounter = nullptr;   // overlay.fps_counter: small fps readout while the overlay is closed
    Setting* abBadge = nullptr;      // overlay.ab_badge: "B" tag while side B is active and the overlay is closed
    Setting* debugMode = nullptr;    // overlay.debug_mode: shows the Flag::Debug settings, the A/B comparison, diagnostics
    Setting* showAdvanced = nullptr; // overlay.show_advanced: shows the Flag::Advanced settings (hidden: the overlay's checkbox)
    Setting* tintDraws = nullptr;    // debug.tint_draws: "off", "kind", "pass" (d3d9/draws.h TintMode)
    Setting* debugCrashMarkers = nullptr;  // debug.crash_markers: VK_AMD_buffer_marker events around the regions, logged after a device loss
    Setting* debugRegions = nullptr;  // debug.regions: command buffer labels for graphics captures (d3d9::Device::setRegionsEnabled)
    Setting* debugNames = nullptr;    // debug.names: VK_EXT_debug_utils object names (restart; vk/debug_names.h)
    Setting* recordThreads = nullptr;  // debug.record_threads: the opaque passes' draws recorded on worker threads (d3d9::Device::setRecordThreads)
    Setting* ubershaderSkip = nullptr;   // debug.ubershader_skip: classes of material shaders kept translated (bisecting)
    Setting* ubershader = nullptr;       // debug.ubershader: the GLSL material shader for the engine's material pixel shaders (d3d9::Device::setUbershader)
    Setting* materialSources = nullptr;  // debug.material_sources: the engine's request of each material shader (engine::matsrc)
    Setting* reverseZ = nullptr;     // debug.reverse_z: reversed infinite depth in the main view (d3d9::Device::setReverseZ)
    Setting* batchDraws = nullptr;   // debug.batch_draws: the opaque passes' draws grouped by pipeline (d3d9::Device::setBatching)
    Setting* splitVerify = nullptr;      // debug.split_verify: check every split draw's window blocks against the register file (d3d9::Device::setSplitVerify)
    Setting* injectView = nullptr;   // debug.inject_view: "off", "depth", "tint" (enh/debug_view.h, an injected pass)
    Setting* rtAllow = nullptr;      // rt.allow: enable the ray tracing extensions at device creation (restart)
    Setting* rtReflections = nullptr; // rt.reflections: "off", "low", "medium", "high" (the water's ray-traced reflections, needs RayQuery)
    Setting* rtDebug = nullptr;      // rt.debug: "off", "instances", "depth", "normals" (enh/rt_debug.h, needs RayQuery)
    Setting* rtReflectionTweaks = nullptr;  // rt.reflection_tweaks: hidden diagnostics bitmask of the water's RT reflections (enh/water.h)
    Setting* rtLodScale = nullptr;     // rt.lod_scale: multiplies both ranges (0 = ladder off)
    Setting* rtMemoryBudget = nullptr; // rt.memory_budget (MB, 0 = none): the ladder tightens when the AS memory would exceed it
    Setting* rtPreset = nullptr;       // rt.preset: "low", "medium", "high", "custom" (rtPresets())
    Setting* rtSceneViews = nullptr; // rt.scene_views: "all", "main", "shadow": which views feed the RT scene (hidden, diagnostics)
    Setting* rtTableEntries = nullptr; // rt.table_entries: F8-0 persistent RT scene entries for static opaque mesh draws (Full, Debug; frame-ownership.md 7.20)
    Setting* rtTableCompare = nullptr; // debug.rt_table_compare: check frames diffing the entries against the full observation
    // enhanced water (enh/water.h, docs/research/water.md)
    Setting* waterEnabled = nullptr;      // water.enabled
    Setting* waterQuality = nullptr;      // water.quality: "low", "medium", "high"
    Setting* waterReflections = nullptr;  // water.reflections: "sky" (cube only), "ssr"
    Setting* waterOcean = nullptr;        // water.ocean: "off", "gerstner", "fft" (the open sea's wave grid + lower coast)
    Setting* waterOceanQuality = nullptr; // water.ocean_quality: "low", "medium", "high" (FFT tiles)
    Setting* waterSeaState = nullptr;     // water.sea_state: "auto" (weather), "calm", "moderate", "rough", "storm"
    Setting* waterWindDirection = nullptr;  // water.wind_direction: degrees, -1 = auto
    Setting* waterWaveScale = nullptr;    // water.wave_scale: multiplier
    Setting* waterDebug = nullptr;        // water.debug: "off", "shore_field", "depth", "normals", "reflection"
    // enhanced sun shadows (enh/shadows.h, docs/research/shadows.md)
    Setting* shadowsEnabled = nullptr;    // shadows.enabled
    Setting* shadowsFilter = nullptr;     // shadows.filter: "low", "medium", "high" (bilinear taps per axis 2 / 3 / 4)
    Setting* shadowsBias = nullptr;       // shadows.bias: constant depth bias (texels of a 45° slope)
    Setting* shadowsDebug = nullptr;      // shadows.debug: "off", "cascades"
    Setting* shadowsMapSize = nullptr;    // shadows.map_size: "auto" (4096 with enhanced shadows, else the game's), "game", "1024", "2048", "4096"
    Setting* shadowsRange = nullptr;      // shadows.range: metres, 0 = auto (100 with enhanced shadows, else the game's)
    Setting* shadowsRt = nullptr;         // shadows.rt: "off", "near", "extended" (ray-traced sun shadows near the camera, enh/rt_shadows)
    Setting* shadowsRtQuality = nullptr;  // shadows.rt_quality: "low", "medium", "high"
    Setting* shadowsRtRange = nullptr;    // shadows.rt_range: metres, 0 = from the mode
    // ray-traced global illumination (enh/gi.h, docs/research/rtgi.md; needs RayQuery)
    Setting* giEnabled = nullptr;         // gi.enabled
    Setting* giQuality = nullptr;         // gi.quality: "low", "medium", "high" (probes traced per frame)
    Setting* giStrength = nullptr;        // gi.strength: 0 = the engine's ambient .. 1 = GI
    Setting* giLightmaps = nullptr;       // gi.lightmaps: "keep", "replace"
    Setting* giScreen = nullptr;          // gi.screen: "off", "low", "medium", "high" (stage C screen probes, needs the G-buffer)
    Setting* giFoliage = nullptr;         // gi.foliage: "off" / "near" / "far": leaf cards in the ray-traced scene (rtgi.md 22)
    Setting* giDebug = nullptr;           // gi.debug: "off", "irradiance", "cascades", "vertex_ao"
    // G-buffer / motion vectors / GTAO (enh/gbuffer.h, docs/research/rtgi.md 10)
    Setting* aoEnabled = nullptr;         // ao.enabled: GTAO on the ambient light
    Setting* giReference = nullptr;       // gi.reference (debug): the reference path tracer's view (docs/research/path-tracing.md)
    Setting* giReferenceSpp = nullptr;    // gi.reference_spp: samples per pixel and frame
    Setting* giReferenceMax = nullptr;    // gi.reference_max: samples until the accumulation stops and is dumped
    Setting* giReferenceBounces = nullptr;  // gi.reference_bounces: the most bounces of a path
    Setting* aoMode = nullptr;            // ao.mode: "replace" (the game's SSAO off), "add"
    Setting* aoStrength = nullptr;        // ao.strength: 0..1
    Setting* gbufferDebug = nullptr;      // gbuffer.debug: "off", "normals", "motion", "ao"
    // Full-owned lighting (enh/light_table, docs/research/gpu-scene.md 9.7)
    Setting* lightingClustered = nullptr; // lighting.clustered (restart, debug): the light table + cluster grid; the material variants take their point lights from it
    Setting* lightingFullLit = nullptr;   // lighting.full_lit (debug): lit draws take all point lights from the table (one 3-light permutation; gpu-scene.md 9.8)
    Setting* lightingEnabled = nullptr;   // lighting.enabled (debug): run-time switch of the table (A/B)
    Setting* lightingStyle = nullptr;     // lighting.style: "original" (the engine's distance law) | "physical"
    Setting* lightingDebug = nullptr;     // lighting.debug: "off" | "clusters" | "slots" | "shadows"
    Setting* lightingShadows = nullptr;   // lighting.light_shadows: "off" | "nearby" | "many" (ray-traced point-light shadows on the table's lights)
    Setting* lightingShadowQuality = nullptr;  // lighting.light_shadow_quality: "low" | "medium" | "high"
    // material pack (enh/material_pack, docs/research/material-pack.md)
    Setting* materialsPack = nullptr;        // materials.pack (restart): load the pack folder; its materials get PBR specular / reflections
    Setting* materialsQuality = nullptr;     // materials.quality: "off" | "low" | "medium" | "high"
    Setting* materialsRtReflections = nullptr; // materials.rt_reflections: "low" | "medium" | "high" (traced reflections at materials.quality = high)
    Setting* materialsRtRange = nullptr;     // materials.rt_range: metres a reflection ray travels
    Setting* materialsGlass = nullptr;       // materials.glass: traced glass panes (windows reflect, bottles refract) with the traced reflections
    Setting* materialsRainWetness = nullptr; // materials.rain_wetness: how strongly rain wets the pack's surfaces (traced reflections on)
    Setting* materialsRainForce = nullptr;   // materials.rain_force (debug): a forced rain share
    Setting* materialsBudget = nullptr;      // materials.budget (restart): MiB of decoded pack textures
    Setting* materialsDerive = nullptr;      // materials.derive_from_specular: roughness from the engine's Specular / SpecularPower roles for draws without a pack entry
    Setting* materialsAlternate = nullptr;   // materials.alternate: test switch, the pack variants on / off every N frames (exact "no entry = unchanged" proof)
    Setting* materialsDebug = nullptr;       // materials.debug: "off" | "roughness" | "metalness" | "orm" | "coverage"
    // upscaling / anti-aliasing (enh/upscale.h, docs/research/upscaling.md)
    Setting* upscaleObjectMotion = nullptr;  // upscale.object_motion: motion vectors of characters and moving objects (enh/object_motion)
    Setting* upscaleProvider = nullptr;   // upscale.provider: "fsr3" (FSR 3.1, default) or "fsr4" (the ML upscaler of the FSR SDK 2.4)
    Setting* upscaleReactive = nullptr;   // upscale.reactive: FSR's automatic reactive mask (particles, rain, spells)
    Setting* upscaleSharpness = nullptr;  // upscale.sharpness: 0..1, FSR's RCAS sharpening pass
    Setting* upscaleExposure = nullptr;   // upscale.exposure: advanced, "engine" (the game's tone map exposure, default) or "auto" (FSR's own)
    Setting* upscaleJitterFlip = nullptr; // upscale.jitter_flip: debug, "none", "x", "y", "xy" (the geometry jitter's sign)
    Setting* upscaleCutoutThicken = nullptr;  // upscale.cutout_thicken: advanced, cut-out strands kept about a render pixel wide (0 = off)
    Setting* upscaleAlphaDither = nullptr;   // upscale.alpha_dither: debug, hashed alpha test on the jittered draws (thin cut-outs)
    // frame generation (enh/framegen.h, docs/research/frame-generation.md)
    Setting* framegenMode = nullptr;      // framegen.mode: "off", "fsr3" (FSR 3 frame generation), "fsr4" (the ML provider, where offered)
    Setting* framegenManual = nullptr;    // framegen.manual: debug, generated frames presented by us (stage F2; the AMD swapchain is F3)
    Setting* framegenDebug = nullptr;     // framegen.debug: debug view of the inputs / the generated frame
    Setting* upscaleMode = nullptr;       // upscale.mode: "off", "jitter_only" (debug), "native_aa" (FSR 3.1 at the output size)
    // detail distances (engine/lod.h, docs/research/lod.md): multipliers, 1 = the game's
    Setting* lodTrees = nullptr;          // lod.trees: SpeedTree LOD + billboard fade distances
    Setting* lodMeshes = nullptr;         // lod.meshes: mesh LOD ranges (eCMesh_PS)
    Setting* rtShadowSync = nullptr;      // debug.rt_shadow_sync: cascades every frame while the RT shadows are on
    Setting* rtSkin = nullptr;            // debug.rt_skin: characters skinned by compute for the RT scene
    Setting* rtSkinVerify = nullptr;      // debug.rt_skin_verify: compare it with the vertex shader capture
    Setting* nativeFrameTap = nullptr;    // native.frame_tap: the main view's render ops + draw attribution (engine-frame N5.0)
    Setting* nativeOwnedObserve = nullptr;  // native.owned_observe: frame ownership F0, the RequestRenderOps hooks in observe mode (frame-ownership.md)
    Setting* nativeStaticOps = nullptr;   // native.static_ops: the static mesh z / solid ops drawn by the renderer (engine-frame N5.1)
    Setting* nativeStaticOpsCompare = nullptr;  // native.static_ops_compare: their composed draws diffed against the engine's
    Setting* nativeSceneOps = nullptr;         // native.scene_ops: the rest of the main view's mesh-like work drawn by the renderer (N5.2)
    Setting* nativeSceneOpsCompare = nullptr;  // native.scene_ops_compare: ... composed and diffed against the engine's draws
    Setting* nativeSceneOpsGroups = nullptr;   // native.scene_ops_groups: which N5.2 groups (bit mask, hidden)
    Setting* nativeCoverage = nullptr;         // native.coverage: the per-draw coverage counter in plain Full runs (O1)
    Setting* nativeIllumTable = nullptr;       // native.illum_table: the GPU illumination table + its draw-lighting verification (frame-ownership.md F0; restart)
    Setting* nativeMeshParts = nullptr;        // native.mesh_parts: the GPU mesh part + layout tables and their verification (frame-ownership.md F0; restart)
    Setting* nativeLightingImages = nullptr;   // native.lighting_images: GPU-composed lighting register images + their verification (frame-ownership.md 7.9; restart; needs native.illum_table)
    Setting* nativeMaterialTable = nullptr;    // native.material_table: the GPU material table + its draw verification (frame-ownership.md F0; restart)
    Setting* nativeOwnedWindows = nullptr;     // native.owned_windows: the static draws' register windows read from the material / view tables (frame-ownership.md 7.8; needs native.material_table)
    Setting* nativeSceneDraws = nullptr;       // native.scene_draws: the static main-view z / solid draws as DrawRecords + indirect runs (frame-ownership.md 7.11)
    Setting* nativeOwnedObjects = nullptr;     // native.owned_objects: the static ops' non-instanced mesh draws read VS windows 1 / 2 from the view block / the object table (frame-ownership.md 7.13)
    Setting* nativeOwnedStatic = nullptr;      // native.owned_static: the static z / solid ops of valid records drawn from the records, left out of the engine's lists (frame-ownership.md 7.16)
    Setting* nativeOwnedPersistent = nullptr;  // native.owned_persistent: the owned draws' DrawRecords / DrawBlocks persistent on the GPU, one indirect draw per bucket (frame-ownership.md 7.21)
    Setting* nativeClusterCull = nullptr;      // native.cluster_cull: the GPU frustum cull of the owned draws of clustered meshes (frame-ownership.md 7.34, F5-2)
    Setting* nativeClusters = nullptr;         // native.clusters: static meshes of >= 1,024 triangles drawn from index copies in cluster order, with a GPU cluster table (frame-ownership.md 7.33, F5-1)
    Setting* nativeOwnedCheck = nullptr;       // native.owned_check: every N-th frame (and the one before it) runs the old path and compares
    Setting* nativeOwnedRecords = nullptr;     // native.owned_records: op records built from the static composition and compared with record-derived draws (frame-ownership.md 7.14)
    Setting* nativePictureCheck = nullptr;  // native.picture_check: GPU compare of the owned main view's opaque picture with a composed frame's (frame-ownership.md 7.47)
    Setting* nativeOwnedTrees = nullptr;  // native.owned_trees: the main view's SpeedTree ops owned (frame-ownership.md 7.25, F4-2 / F4-3)
    Setting* nativeClusterCullShadows = nullptr;  // native.cluster_cull_shadows: the cull of the owned draws of clustered meshes in the sun cascades (frame-ownership.md 7.37, F3-2)
    Setting* nativeOwnedInstances = nullptr;  // native.owned_instances: instance lists + hardware batches owned (frame-ownership.md 14.19, F2f)
    Setting* nativeOwnedOrdered = nullptr;  // native.owned_ordered: the order-dependent ops (blended terrain layers, alpha / refracted lists) owned at their place (frame-ownership.md 14.20, F9 + F14)
    Setting* nativeOwnedActors = nullptr;  // native.owned_actors: skinned actor ops owned (frame-ownership.md 7.40, F6-1)
    Setting* nativeOwnedShadows = nullptr;  // native.owned_shadows: the sun cascades' static mesh z ops owned (frame-ownership.md 7.36, F3-1)
    Setting* nativeOwnedVegetation = nullptr;  // native.owned_vegetation: the vegetation ops' instance streams retained in persistent GPU buffers (frame-ownership.md 7.40, F7-1)
    Setting* nativeOwnedVegetationOps = nullptr;  // native.owned_vegetation_ops: the vegetation ops' draws owned (frame-ownership.md 7.42, F7-2)
    Setting* nativeOwnedBillboards = nullptr;  // native.owned_billboards: the tree billboards' ops retained as records and drawn from them (frame-ownership.md 7.17, F4-1)
    Setting* nativeSceneDrawsCheck = nullptr;  // native.scene_draws_check: every N-th frame the record / argument buffers are read back and compared
    Setting* nativeLean = nullptr;             // native.lean: the engine's slot code not run for the composed mesh-like ops (O2a)
    Setting* nativeLeanCheck = nullptr;        // native.lean_check: every N-th frame the engine's code runs as before (compare)
    Setting* nativeRigidMotion = nullptr;      // native.rigid_motion (frame-ownership.md 14.15 b: moving rigid draws' per-object motion from the object table's history, restart)
    Setting* nativeBoneMotion = nullptr;       // native.bone_motion: off | on | compare (frame-ownership.md 14.13: the skinned draws' per-object motion from the bone table's history, restart)
    Setting* nativeOwnFoliage = nullptr;       // native.own_foliage: S-5 grass distance thinning tied to lod.view (Full)
    Setting* nativeRtRecords = nullptr;        // native.rt_records: F8-1 (frame-ownership.md 14.30): the RT scene's sightings of owned draws replayed from their record scripts (Full, Debug)
    Setting* nativeTreeWind = nullptr;         // native.tree_wind: off | on | compare (F4-5, frame-ownership.md 14.17: the RT scene's animated SpeedTrees by compute, no capture; Full)
    Setting* nativeVertexPull = nullptr;       // native.vertex_pull: S-4 vertex pulling of the persistent owned draws (no vertex input: one indirect command per pipeline x state x index binding)
    Setting* nativeDepthZ = nullptr;          // native.depth_z: depth-only z passes (S-6: no pixel shader / colour attachment where nothing reads them; the R32F depth map derived from the hardware depth)
    Setting* nativeOwnPost = nullptr;         // native.own_post: the post chain on our own shaders (S-7: the luminance chain in one dispatch)
    Setting* nativeOwnSky = nullptr;          // native.own_sky: the sky dome (main view + sky cube faces) on our own shaders (frame-ownership.md 14.24, F12)
    Setting* nativeOwnedDecals = nullptr;     // native.owned_decals: decal records in persistent buffers (frame-ownership.md 14.26, row 13)
    Setting* nativeOwnParticles = nullptr;    // native.own_particles: the particle quads expanded on the GPU (frame-ownership.md 14.26, F11)
    Setting* nativeOwnGui = nullptr;          // native.own_gui: the GUI sprite batches on our own shaders (frame-ownership.md 14.24, F13)
    Setting* nativePacketCache = nullptr;     // native.packet_cache: retained draw packets of the main view's static meshes + trees (retained-signals.md 8)
    Setting* shadowStride = nullptr;      // debug.shadow_stride: the sun cascades redrawn every n-th frame (engine::shadowmaps::setUpdateStride)
    Setting* streamStats = nullptr;       // debug.stream_stats: loading work statistics (engine::loadaccel)
    Setting* prefetchStride = nullptr;    // debug.prefetch_stride: the cache-in walk every n-th frame (engine::lod::setPrefetchStride)
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
// Original (the game's look), Low, Medium, High (= the defaults), Ultra, Integrated ("igpu": the 680M table, docs/research/frame-ownership.md 14.11; index 5, shown
// in the overlay on integrated GPUs or while it matches)
const std::vector<Preset>& graphicsPresets();
// the RT quality presets (rt.preset): low / medium / high = the cost knobs of everything that traces (shadows, reflections, GI, material
// reflections, the LOD ladder, the memory limit); matchRtPreset = index of the first whose values all match, -1 = custom
const std::vector<Preset>& rtPresets();
int matchRtPreset(const Registry& reg);
// makes rt.preset say what the members say: the matching preset, else "custom" ("custom" is a state, not a choice: setting it changes nothing;
// the hidden members rt.lod_scale / shadows.rt_range do not count). Called after settings.toml is loaded and after every member change;
// true = rt.preset changed
bool syncRtPreset(Registry& reg);
// sets the preset's values (locked settings keep theirs)
void applyPreset(Registry& reg, const Preset& preset);
// Integrated GPUs (caps::Report::integratedGpu): the first run on such a GPU applies the "igpu" preset to every setting still at its default
// (settings the player or a file already set keep their value; locked ones too), once (system.igpu_preset_done). Returns the changes as
// "key: old -> new" lines (empty = nothing applied: already done, or no setting left at its default)
std::vector<std::string> applyIntegratedDefaults(Registry& reg);
// index of the first preset whose values all match the current ones, -1 = custom
int matchPreset(const Registry& reg);
// resets every setting of an overlay tab to its default (locked ones stay); returns how many changed
std::size_t resetTab(Registry& reg, std::string_view tab);
// the same for an overlay page (Spec::page); returns how many changed
std::size_t resetPage(Registry& reg, std::string_view page);

}  // namespace rvk::settings
