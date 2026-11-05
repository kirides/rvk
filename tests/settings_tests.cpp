// Player settings registry (src/core/settings.*): values, text form, callbacks, locks, A/B, settings.toml round trip.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include "core/config.h"
#include "core/settings.h"
#include "test.h"

using namespace rvk;
using namespace rvk::settings;

namespace {

struct Fixture {
    Registry reg;
    Setting* flag;
    Setting* count;
    Setting* ratio;
    Setting* mode;
    Setting* restart;
    Setting* hotkey;
    Fixture() {
        flag = &reg.add({.key = "water.enhanced", .label = "Enhanced water", .category = "Water", .type = Type::Bool});
        count = &reg.add({.key = "water.steps", .label = "Steps", .category = "Water", .type = Type::Int, .defaultValue = 16,
                          .min = 1, .max = 64});
        ratio = &reg.add({.key = "rt.max_distance", .label = "Distance", .category = "RT", .type = Type::Float,
                          .defaultValue = 0.5, .min = 0.0, .max = 2.0});
        mode = &reg.add({.key = "vulkan.vsync", .label = "VSync", .category = "Display", .type = Type::Enum, .defaultValue = 0,
                         .options = {"engine", "on", "off"}});
        restart = &reg.add({.key = "vulkan.adapter", .label = "GPU", .category = "Display", .type = Type::Int,
                            .defaultValue = -1, .min = -1, .max = 15, .flags = Flag::Restart});
        hotkey = &reg.add({.key = "overlay.toggle_key", .label = "Key", .category = "Overlay", .type = Type::Enum,
                           .defaultValue = 1, .options = {"none", "f10", "f11"}, .flags = Flag::NoAB});
    }
};

}  // namespace

TEST(settings_register_and_defaults) {
    Fixture f;
    CHECK(f.reg.size() == 6);
    CHECK(f.reg.find("water.steps") == f.count);
    CHECK(f.reg.find("water.nope") == nullptr);
    CHECK(!f.flag->asBool());
    CHECK(f.count->asInt() == 16);
    CHECK(f.ratio->asFloat() == 0.5f);
    CHECK(f.mode->asEnum() == "engine");
    CHECK(f.restart->asInt() == -1);
    // duplicates return the first, malformed keys a hidden dummy
    Setting& dup = f.reg.add({.key = "water.steps", .type = Type::Bool});
    CHECK(&dup == f.count);
    Setting& bad = f.reg.add({.key = "NoDot", .type = Type::Bool});
    CHECK((bad.spec().flags & Flag::Hidden) != 0);
    Setting& reserved = f.reg.add({.key = "ab.x", .type = Type::Bool});
    CHECK((reserved.spec().flags & Flag::Hidden) != 0);
}

TEST(settings_set_and_clamp) {
    Fixture f;
    CHECK(f.reg.set(*f.count, 100) == Result::Ok);
    CHECK(f.count->asInt() == 64);
    CHECK(f.reg.set(*f.count, 2.6) == Result::Ok);
    CHECK(f.count->asInt() == 3);
    CHECK(f.reg.set(*f.ratio, -1.0) == Result::Ok);
    CHECK(f.ratio->value() == 0.0);
    CHECK(f.reg.set(*f.mode, 3) == Result::BadValue);
    CHECK(f.reg.set(*f.mode, 1.5) == Result::BadValue);
    CHECK(f.reg.set(*f.mode, 2) == Result::Ok);
    CHECK(f.mode->asEnum() == "off");
    CHECK(f.reg.set("nope.key", 1) == Result::UnknownKey);
    CHECK(f.reg.dirty());
}

TEST(settings_text_form) {
    Fixture f;
    CHECK(f.reg.setText("water.enhanced", "on") == Result::Ok);
    CHECK(f.flag->asBool());
    CHECK(f.reg.setText("water.enhanced", " FALSE ") == Result::Ok);
    CHECK(!f.flag->asBool());
    CHECK(f.reg.setText("water.enhanced", "maybe") == Result::BadValue);
    CHECK(f.reg.setText("vulkan.vsync", "OFF") == Result::Ok);
    CHECK(f.mode->asEnum() == "off");
    CHECK(f.reg.setText("vulkan.vsync", "\"on\"") == Result::Ok);
    CHECK(f.mode->asEnum() == "on");
    CHECK(f.reg.setText("vulkan.vsync", "0") == Result::Ok);  // index
    CHECK(f.mode->asEnum() == "engine");
    CHECK(f.reg.setText("vulkan.vsync", "sometimes") == Result::BadValue);
    CHECK(f.reg.setText("water.steps", "12") == Result::Ok);
    CHECK(f.count->asInt() == 12);
    CHECK(f.reg.setText("water.steps", "12x") == Result::BadValue);
    CHECK(f.reg.setText("rt.max_distance", "1.25") == Result::Ok);
    CHECK(f.reg.text(*f.ratio) == "1.25");
    CHECK(f.reg.text(*f.flag) == "false");
    CHECK(f.reg.text(*f.count) == "12");
    CHECK(f.reg.text(*f.mode) == "engine");
}

TEST(settings_callbacks_dispatch) {
    Fixture f;
    int calls = 0;
    double seen = 0;
    f.reg.onChange(*f.count, [&](const Setting& s) {
        ++calls;
        seen = s.value();
    });
    CHECK(f.reg.dispatch() == 0);
    f.reg.set(*f.count, 20);
    f.reg.set(*f.count, 24);  // coalesced: one callback with the latest value
    CHECK(calls == 0);        // callbacks run at dispatch, not inside set
    CHECK(f.reg.dispatch() == 1);
    CHECK(calls == 1);
    CHECK(seen == 24);
    f.reg.set(*f.count, 24);  // no change, no callback
    CHECK(f.reg.dispatch() == 0);
    CHECK(calls == 1);
}

TEST(settings_lock_override) {
    Fixture f;
    f.reg.set(*f.mode, 1);  // the player's "on"
    f.reg.lock(*f.mode, 2, "config.toml [vulkan] vsync");
    CHECK(f.mode->locked());
    CHECK(f.mode->asEnum() == "off");
    CHECK(f.reg.set(*f.mode, 0) == Result::Locked);
    CHECK(f.reg.lockReason(*f.mode) == "config.toml [vulkan] vsync");
    // the file keeps the player's value, not the override
    const std::string toml = f.reg.toToml();
    CHECK_MSG(toml.find("vsync = \"on\"") != std::string::npos, toml);
    // locked settings stay out of A/B
    f.reg.flipAB();
    CHECK(f.mode->asEnum() == "off");
}

TEST(settings_restart_pending) {
    Fixture f;
    f.reg.markStarted();
    CHECK(!f.restart->restartPending());
    f.reg.set(*f.restart, 1);
    CHECK(f.restart->restartPending());
    CHECK(f.restart->startValue() == -1);
    f.reg.set(*f.restart, -1);
    CHECK(!f.restart->restartPending());
}

TEST(settings_ab_flip) {
    Fixture f;
    // side B starts as the defaults: flipping compares the player's values against plain parity
    f.reg.set(*f.flag, 1);
    f.reg.set(*f.count, 32);
    f.reg.set(*f.hotkey, 2);   // NoAB: stays
    f.reg.set(*f.restart, 0);  // Restart: stays
    f.reg.dispatch();
    CHECK(!f.reg.bActive());
    CHECK(f.reg.abDifferences().size() == 2);
    int changed = 0;
    f.reg.onChange(*f.flag, [&](const Setting&) { ++changed; });
    f.reg.flipAB();
    CHECK(f.reg.bActive());
    CHECK(!f.flag->asBool());
    CHECK(f.count->asInt() == 16);
    CHECK(f.hotkey->asEnum() == "f11");
    CHECK(f.restart->asInt() == 0);
    CHECK(f.reg.dispatch() == 2);
    CHECK(changed == 1);
    // edit side B, flip back: A is intact
    f.reg.set(*f.ratio, 1.5);
    f.reg.flipAB();
    CHECK(!f.reg.bActive());
    CHECK(f.flag->asBool());
    CHECK(f.count->asInt() == 32);
    CHECK(f.ratio->value() == 0.5);
    CHECK(f.reg.otherValue(*f.ratio) == 1.5);
    // copy A over B: no differences left
    f.reg.copyActiveToOther();
    CHECK(f.reg.abDifferences().empty());
    f.reg.resetOther();
    CHECK(f.reg.abDifferences().size() == 2);
}

TEST(settings_toml_roundtrip) {
    Fixture f;
    CHECK(f.reg.toToml().find("\n[") == std::string::npos);  // all defaults: no tables (comments only)
    f.reg.set(*f.flag, 1);
    f.reg.set(*f.count, 8);
    f.reg.set(*f.ratio, 2.0);
    f.reg.set(*f.mode, 2);
    f.reg.set(*f.restart, 1);
    f.reg.set(*f.hotkey, 0);
    // side B: vsync on, the rest defaults
    f.reg.flipAB();
    f.reg.set(*f.mode, 1);
    const std::string toml = f.reg.toToml();  // written while B is active: A still goes to the main tables
    CHECK_MSG(toml.find("[water]\nenhanced = true\nsteps = 8\n") != std::string::npos, toml);
    CHECK_MSG(toml.find("[rt]\nmax_distance = 2.0\n") != std::string::npos, toml);
    CHECK_MSG(toml.find("[vulkan]\nvsync = \"off\"\nadapter = 1\n") != std::string::npos, toml);
    CHECK_MSG(toml.find("[overlay]\ntoggle_key = \"none\"\n") != std::string::npos, toml);
    CHECK_MSG(toml.find("[ab.vulkan]\nvsync = \"on\"\n") != std::string::npos, toml);
    CHECK_MSG(toml.find("[ab.water]") == std::string::npos, toml);  // defaults on side B are not written

    Fixture g;
    std::vector<std::string> warnings;
    CHECK(g.reg.fromToml(toml, &warnings));
    CHECK(warnings.empty());
    CHECK(!g.reg.bActive());
    CHECK(g.flag->asBool());
    CHECK(g.count->asInt() == 8);
    CHECK(g.ratio->value() == 2.0);
    CHECK(g.mode->asEnum() == "off");
    CHECK(g.restart->asInt() == 1);
    CHECK(g.hotkey->asEnum() == "none");
    CHECK(g.reg.otherValue(*g.mode) == 1);
    CHECK(g.reg.otherValue(*g.flag) == 0);
    CHECK(g.reg.toToml() == toml);
}

TEST(settings_toml_bad_input) {
    Fixture f;
    std::vector<std::string> warnings;
    CHECK(f.reg.fromToml(R"(
[water]
enhanced = 3
steps = 1000
unknown = 1
[vulkan]
vsync = "sometimes"
[rt]
max_distance = 1
[ab.overlay]
toggle_key = "f11"
)",
                         &warnings));
    CHECK(!f.flag->asBool());         // 3 is not a bool
    CHECK(f.count->asInt() == 64);    // clamped
    CHECK(f.mode->asEnum() == "engine");
    CHECK(f.ratio->value() == 1.0);   // integer accepted for a float
    CHECK(f.hotkey->asEnum() == "f10");
    CHECK_MSG(warnings.size() == 4, std::to_string(warnings.size()));  // enhanced, unknown, vsync, ab.overlay.toggle_key
    warnings.clear();
    CHECK(!f.reg.fromToml("this is [ not toml", &warnings));
    CHECK(warnings.size() == 1);
}

TEST(settings_file_save_load) {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const auto dir = std::filesystem::path(tmp) / L"rvk_settings_test";
    std::filesystem::remove_all(dir);
    const auto file = dir / L"settings.toml";
    Fixture f;
    CHECK(f.reg.load(file));  // missing file: defaults, no error
    f.reg.set(*f.count, 5);
    CHECK(f.reg.dirty());
    std::string err;
    CHECK(f.reg.save(file, &err));
    CHECK(!f.reg.dirty());
    CHECK(!std::filesystem::exists(dir / L"settings.toml.tmp"));
    Fixture g;
    CHECK(g.reg.load(file));
    CHECK(g.count->asInt() == 5);
    CHECK(!g.reg.dirty());
    std::filesystem::remove_all(dir);
}

TEST(settings_renderer_registration) {
    Registry reg;
    const RendererSettings r = registerRendererSettings(reg);
    CHECK(r.vsync != nullptr && r.vsync->asEnum() == "engine");
    CHECK(r.maxFps != nullptr && r.maxFps->asInt() == 0);
    CHECK(r.adapter != nullptr && r.adapter->restart());
    CHECK(r.toggleKey != nullptr && r.toggleKey->asEnum() == "f11");  // as in GD3D11
    CHECK(r.abKey != nullptr && r.abKey->asEnum() == "f10");
    CHECK(r.abBadge->asBool());
    CHECK(!r.fpsCounter->asBool());
    // debug: the tint view is off by default and not part of A/B
    CHECK(r.tintDraws != nullptr && r.tintDraws->asEnum() == "off" && !r.tintDraws->abEligible());
    // ... and so is the RT scene's source switch (diagnostics)
    CHECK(r.rtSceneViews != nullptr && r.rtSceneViews->asEnum() == "all" && (r.rtSceneViews->spec().flags & Flag::Hidden) != 0);
    // ... and the water's RT reflection tweaks (cost measurements)
    CHECK(r.rtReflectionTweaks != nullptr && r.rtReflectionTweaks->asInt() == 0 && (r.rtReflectionTweaks->spec().flags & Flag::Hidden) != 0);
    // ... and Vulkan's exclusive mode on top of exclusive fullscreen's display mode change (diagnostics); the fullscreen mode
    // itself is a player setting that needs the exclusive fullscreen capability
    CHECK(r.exclusiveAcquire != nullptr && r.exclusiveAcquire->asBool() && (r.exclusiveAcquire->spec().flags & Flag::Hidden) != 0);
    CHECK(r.fullscreen != nullptr && r.fullscreen->spec().defaultValue == 0 &&
          r.fullscreen->spec().requirements == rvk::caps::bit(rvk::caps::Cap::FullScreenExclusive));
    // the menu frame rate limit is a player setting (60 fps)
    CHECK(r.menuFps != nullptr && r.menuFps->value() == 60 && (r.menuFps->spec().flags & Flag::Hidden) == 0);
    for (const Setting* s : reg.all()) {
        // Hidden settings are the checkbox, the preset's invisible members, or developer switches (which carry Flag::Debug: a stray
        // value in settings.toml does not apply for players)
        const bool hidden = (s->spec().flags & Flag::Hidden) != 0;
        CHECK(!hidden || s == r.showAdvanced || s == r.rtLodScale || s == r.shadowsRtRange || (s->spec().flags & Flag::Debug) != 0);
    }
    // the player defaults: enhanced water (high, RT reflections high) and enhanced shadows (low filter, 2048 maps)
    CHECK(r.waterEnabled->asBool() && r.waterQuality->asEnum() == "high" && r.waterOceanQuality->asEnum() == "high");
    CHECK(r.rtReflections->value() == 3);  // "high" (effective "off" until a device reports ray query)
    CHECK(r.shadowsEnabled->asBool() && r.shadowsFilter->asEnum() == "low" && r.shadowsMapSize->asEnum() == "2048");
    // debug mode: off by default, the developer settings are gated, the A/B comparison belongs to it
    CHECK(r.debugMode != nullptr && !r.debugMode->asBool() && !reg.debugMode());
    for (const Setting* s : {r.abKey, r.abBadge, r.tintDraws, r.injectView, r.rtDebug, r.waterDebug, r.shadowsDebug}) {
        CHECK((s->spec().flags & Flag::Debug) != 0 && s->gated());
    }
    CHECK(!r.waterEnabled->gated() && (r.debugMode->spec().flags & Flag::Debug) == 0);
}

TEST(settings_presets_and_tabs) {
    Registry reg;
    const RendererSettings r = registerRendererSettings(reg);
    const auto& presets = graphicsPresets();
    CHECK(presets.size() == 5);
    for (const Preset& p : presets) {
        for (const auto& [key, text] : p.values) {
            const Setting* s = reg.find(key);
            double v = 0.0;
            CHECK_MSG(s != nullptr && Registry::parseText(s->spec(), text, v), p.name + ": " + key + " = " + text);
        }
    }
    CHECK(matchPreset(reg) == 3);  // the defaults are "High"
    applyPreset(reg, presets[0]);
    CHECK(matchPreset(reg) == 0 && !r.waterEnabled->asBool() && !r.shadowsEnabled->asBool());
    applyPreset(reg, presets[4]);
    CHECK(matchPreset(reg) == 4 && r.shadowsMapSize->asEnum() == "4096");
    CHECK(reg.set(*r.waterWaveScale, 2) == Result::Ok);  // advanced settings are not part of the presets
    CHECK(matchPreset(reg) == 4);
    CHECK(reg.setText("shadows.filter", "medium") == Result::Ok);
    CHECK(matchPreset(reg) == -1);  // custom
    // "Reset to defaults" of a tab: only that tab
    CHECK(reg.set(*r.maxFps, 60) == Result::Ok);
    CHECK(resetTab(reg, kTabGraphics) > 0);
    CHECK(matchPreset(reg) == 3 && r.waterWaveScale->value() == 1 && r.maxFps->value() == 60);
    CHECK(resetTab(reg, kTabGraphics) == 0);
    // every setting is in one of the tabs; the Debug tab holds the debug-mode settings (and hidden diagnostics)
    for (const Setting* s : reg.all()) {
        const std::string& tab = s->spec().tab;
        const bool debug = (s->spec().flags & Flag::Debug) != 0, hidden = (s->spec().flags & Flag::Hidden) != 0;
        CHECK_MSG(tab == kTabDisplay || tab == kTabGraphics || tab == kTabSystem || tab == kTabDebug, s->key());
        CHECK_MSG(debug == (tab == kTabDebug) || hidden, s->key());
    }
}

TEST(settings_overlay_metadata) {
    // every registered setting has a label, a tooltip, a known page and a section; Debug-tier settings are on the Debug page only
    Registry reg;
    registerRendererSettings(reg);
    const auto& pages = pageOrder();
    for (const Setting* s : reg.all()) {
        const Spec& sp = s->spec();
        const std::string page = pageOf(sp);
        CHECK_MSG(!sp.label.empty() && !sp.description.empty(), s->key());
        CHECK_MSG(std::ranges::find(pages, page) != pages.end(), s->key() + " page " + page);
        CHECK_MSG(!sectionOf(sp).empty(), s->key());
        if (tierOf(sp) == Tier::Debug) CHECK_MSG(page == kPageDebug, s->key());
        if (page == kPageDebug) CHECK_MSG(tierOf(sp) != Tier::Basic, s->key());
    }
    // every preset (master quality, rt) names existing settings with valid values
    for (const auto* list : {&graphicsPresets(), &rtPresets()}) {
        for (const Preset& p : *list) {
            for (const auto& [key, text] : p.values) {
                const Setting* s = reg.find(key);
                double v = 0.0;
                CHECK_MSG(s != nullptr && Registry::parseText(s->spec(), text, v), p.name + ": " + key);
            }
        }
    }
    // a page reset touches only that page
    CHECK(reg.setText("vulkan.max_fps", "60") == Result::Ok && reg.setText("water.wave_scale", "2") == Result::Ok);
    CHECK(resetPage(reg, kPageDisplay) == 1);
    CHECK(reg.find("water.wave_scale")->value() == 2);
}

TEST(settings_debug_mode) {
    Registry reg;
    const RendererSettings r = registerRendererSettings(reg);
    // outside debug mode a debug view applies as its default; the player's value is kept (file, API)
    CHECK(reg.setText("water.debug", "normals") == Result::Ok);
    CHECK(r.waterDebug->value() != 0 && r.waterDebug->asEnum() == "off");
    reg.dispatch();
    std::size_t calls = 0;
    reg.onChange(*r.waterDebug, [&](const Setting&) { ++calls; });
    CHECK(reg.set(*r.debugMode, 1) == Result::Ok);
    CHECK(reg.debugMode() && !r.waterDebug->gated() && r.waterDebug->asEnum() == "normals");
    reg.dispatch();
    CHECK(calls == 1);  // its effective value changed: features hear about it
    // leaving debug mode returns to side A and gates the views again
    CHECK(reg.set(*r.waterEnabled, 0) == Result::Ok);
    reg.flipAB();
    CHECK(reg.bActive() && r.waterEnabled->asBool());
    CHECK(reg.set(*r.debugMode, 0) == Result::Ok);
    CHECK(!reg.bActive() && !r.waterEnabled->asBool() && r.waterDebug->asEnum() == "off");
    reg.dispatch();
    CHECK(calls == 2);
    // settings.toml switches it like any other setting
    CHECK(reg.fromToml("[overlay]\ndebug_mode = true\n"));
    CHECK(reg.debugMode() && r.waterDebug->asEnum() == "normals");
    // an enum with numeric option names also reads from a TOML integer (run.ps1 -Setting 'shadows.map_size=1024')
    CHECK(reg.fromToml("[shadows]\nmap_size = 1024\n"));
    CHECK(r.shadowsMapSize->asEnum() == "1024");
    // a registry without a debug switch applies everything
    Registry plain;
    Setting& view = plain.add({.key = "x.view", .label = "View", .category = "Debug", .type = Type::Bool, .flags = Flag::Debug});
    CHECK(plain.debugMode() && !view.gated());
    CHECK(plain.set(view, 1) == Result::Ok && view.asBool());
}

TEST(config_overlay_options) {
    const Config c = parseConfig(R"(
[overlay]
enabled = false
[debug]
overlay_in_screenshots = true
commands = ["450: overlay open", "600: set vulkan.max_fps 30"]
)");
    CHECK(!c.overlay);
    CHECK(c.overlayInScreenshots);
    CHECK(c.commands.size() == 2);
    CHECK(c.commands[1] == "600: set vulkan.max_fps 30");
}

// renderer.mode (core/config.h RenderMode): Wrapper <-> Full live, Original <-> the others after a restart; A/B flips it;
// config.toml's non-default mode locks it
TEST(settings_renderer_mode) {
    Registry reg;
    const RendererSettings r = registerRendererSettings(reg);
    Setting& m = *r.rendererMode;
    CHECK(m.key() == "renderer.mode" && m.spec().tab == kTabSystem && !m.restart());
    CHECK(m.asEnum() == "wrapper" && static_cast<RenderMode>(m.asInt()) == kDefaultRenderMode);
    CHECK(m.spec().options.size() == 3 && m.spec().options[0] == "original" && m.spec().options[2] == "full");
    reg.markStarted();
    // wrapper -> full: no restart, A/B eligible
    CHECK(reg.setText("renderer.mode", "full") == Result::Ok);
    CHECK(m.asEnum() == "full" && !m.restartPending() && m.abEligible());
    // -> original: restart pending (another backend); back to full / wrapper: none
    CHECK(reg.setText("renderer.mode", "original") == Result::Ok);
    CHECK(m.restartPending());
    CHECK(reg.setText("renderer.mode", "wrapper") == Result::Ok);
    CHECK(!m.restartPending());
    // a session that started in Original: either Vulkan mode needs a restart
    CHECK(reg.setText("renderer.mode", "original") == Result::Ok);
    reg.markStarted();
    CHECK(!m.restartPending());
    CHECK(reg.setText("renderer.mode", "full") == Result::Ok && m.restartPending());
    CHECK(reg.setText("renderer.mode", "wrapper") == Result::Ok && m.restartPending());
    // A/B: side B = full flips with F10
    CHECK(reg.setText("renderer.mode", "wrapper") == Result::Ok);
    reg.markStarted();
    reg.flipAB();
    CHECK(reg.setText("renderer.mode", "full") == Result::Ok);
    reg.flipAB();
    CHECK(m.asEnum() == "wrapper");
    reg.flipAB();
    CHECK(m.asEnum() == "full");
    reg.flipAB();
    // settings.toml round trip
    CHECK(reg.fromToml("[renderer]\nmode = \"full\"\n") && m.asEnum() == "full");
    CHECK(reg.toToml().find("[renderer]\nmode = \"full\"") != std::string::npos);
    // config.toml's lock (bootstrap: configModeLock): fixed, not part of A/B
    const Config cfg = parseConfig("[renderer]\nmode = \"original\"\n");
    const auto lock = configModeLock(cfg);
    CHECK(lock == RenderMode::Original);
    reg.lock(m, static_cast<double>(*lock), "config.toml [renderer] mode");
    CHECK(m.locked() && m.asEnum() == "original" && !m.abEligible());
    CHECK(reg.setText("renderer.mode", "wrapper") == Result::Locked);
    reg.markStarted();
    CHECK(sessionBackend(cfg, static_cast<RenderMode>(m.startValue())) == Mode::Passthrough);
    // the player's own value is what settings.toml keeps
    CHECK(reg.toToml().find("mode = \"full\"") != std::string::npos);
}

TEST(rt_presets_match_defaults_and_apply) {
    settings::Registry reg;
    settings::registerRendererSettings(reg);
    // a fresh registry = Medium (the preset's members all at their defaults)
    CHECK(settings::matchRtPreset(reg) == 1);
    const auto& presets = settings::rtPresets();
    CHECK(presets.size() == 3);
    for (const auto& p : presets) {
        for (const auto& kv : p.values) CHECK(reg.find(kv.first) != nullptr);
    }
    settings::applyPreset(reg, presets[0]);
    CHECK(settings::matchRtPreset(reg) == 0);
    CHECK(reg.find("rt.lod_scale")->value() > 0.29 && reg.find("rt.lod_scale")->value() < 0.31);
    CHECK(reg.find("rt.memory_budget")->value() == 384);
    settings::applyPreset(reg, presets[2]);
    CHECK(settings::matchRtPreset(reg) == 2 && reg.find("rt.lod_scale")->value() == 0.0);
    reg.setText("gi.quality", "low");  // a hand change: no preset matches
    CHECK(settings::matchRtPreset(reg) == -1);
}

// settings that were taken out (docs/research/settings-audit.md clean-up): an old settings.toml loads, one line per key, the rest applies
TEST(settings_cleanup_removed_keys_still_load) {
    using namespace rvk::settings;
    Registry reg;
    registerRendererSettings(reg);
    std::vector<std::string> warnings;
    CHECK(reg.fromToml("[lighting]\ntable_complete = true\ntrust_table = true\n[shadows]\nsoftness = 2.0\nfilter = \"high\"\n"
                       "[materials]\npuddles = 0.3\n[native]\nretained = true\n[debug]\nfast_sort = 0\n", &warnings));
    CHECK(warnings.size() == 6);
    for (const auto& w : warnings) CHECK_MSG(w.find("was removed") != std::string::npos, w);
    CHECK(reg.find("shadows.filter")->asEnum() == "high");
    CHECK(reg.find("shadows.softness") == nullptr && reg.find("lighting.trust_table") == nullptr);
    CHECK(reg.toToml().find("softness") == std::string::npos);
}

TEST(rt_preset_follows_the_members_after_a_load) {
    using namespace rvk::settings;
    Registry reg;
    registerRendererSettings(reg);
    std::vector<std::string> warnings;
    // run.ps1's baseline: preset medium + reflections medium is not Medium (the preset's reflections are high)
    CHECK(reg.fromToml("[rt]\npreset = \"medium\"\nreflections = \"medium\"\n", &warnings));
    CHECK(reg.find("rt.preset")->asEnum() == "medium");  // as loaded (RT unavailable here: effective follows the default anyway)
    CHECK(matchRtPreset(reg) == -1);
    CHECK(syncRtPreset(reg));
    CHECK(reg.text(*reg.find("rt.preset")) == "custom");
    CHECK(reg.setText("rt.reflections", "high") == Result::Ok);  // back to the members of Medium
    CHECK(syncRtPreset(reg) && reg.text(*reg.find("rt.preset")) == "medium");
    CHECK(!syncRtPreset(reg));
    // the hidden members do not make a preset custom
    CHECK(reg.setText("rt.lod_scale", "0.5") == Result::Ok);
    CHECK(matchRtPreset(reg) == 1);
}

TEST(settings_needs_resolution) {
    using namespace rvk::settings;
    Registry reg;
    const auto boolean = [&](const char* key, std::vector<Need> needs, std::uint32_t flags = 0) -> Setting& {
        return reg.add({.key = key, .label = key, .category = "t", .type = Type::Bool, .defaultValue = 0, .flags = flags, .needs = std::move(needs)});
    };
    boolean("a.root", {});
    Setting& mid = reg.add({.key = "a.mid", .label = "Mid", .category = "t", .type = Type::Enum, .defaultValue = 0,
                            .options = {"off", "low", "high"}, .optionLabels = {"Off", "Low", "High"}, .needs = {{"a.root", "true"}}});
    boolean("a.leaf", {{"a.mid", "high"}});
    boolean("a.neg", {{"a.mid", "off", true}});
    boolean("a.cycle1", {{"a.cycle2", "true"}});
    boolean("a.cycle2", {{"a.cycle1", "true"}});
    boolean("a.restart", {}, Flag::Restart);
    boolean("a.needs_restart", {{"a.restart", "true"}});
    boolean("a.manual", {{"a.root", "true", false, false}});
    boolean("a.cap", {}, 0);
    reg.add({.key = "a.capdep", .label = "c", .category = "t", .type = Type::Bool, .requirements = caps::bit(caps::Cap::RayQuery), .unmetValue = 0});
    boolean("a.needs_cap", {{"a.capdep", "true"}});

    // a chain: the leaf switches mid to High and root on; the toast list names both, nearest first
    CHECK(reg.needsReason(*reg.find("a.leaf")).find("Mid must be High") != std::string::npos);
    auto r = reg.setWithNeeds("a.leaf", "true");
    CHECK(r.result == Result::Ok && r.blocked.empty() && r.changes.size() == 2);
    CHECK(r.changes[0].key == "a.root" && r.changes[1].key == "a.mid" && r.changes[1].to == "high");
    CHECK(reg.needsReason(*reg.find("a.leaf")).empty());
    // turning a prerequisite off changes nothing else: the dependents are greyed with the reason
    CHECK(reg.setText("a.root", "false") == Result::Ok);
    CHECK(reg.find("a.leaf")->asBool() && reg.find("a.mid")->value() == 2);
    CHECK(reg.needsReason(*reg.find("a.leaf")).find("a.root must be on") != std::string::npos);
    // turning a dependent off or setting an unrelated one touches nothing
    r = reg.setWithNeeds("a.leaf", "false");
    CHECK(r.changes.empty() && !reg.find("a.root")->asBool());
    // a negated need: "a.mid != off"
    CHECK(reg.setText("a.mid", "off") == Result::Ok);
    CHECK(!reg.needsReason(*reg.find("a.neg")).empty());
    r = reg.setWithNeeds("a.neg", "true");
    CHECK(r.changes.size() == 2 && reg.find("a.mid")->value() == 1 && reg.find("a.root")->asBool());
    // a cycle terminates and still switches both on
    r = reg.setWithNeeds("a.cycle1", "true");
    CHECK(r.result == Result::Ok && reg.find("a.cycle2")->asBool());
    // a restart-only prerequisite is set and flagged
    r = reg.setWithNeeds("a.needs_restart", "true");
    CHECK(r.changes.size() == 1 && r.changes[0].restart && reg.find("a.restart")->asBool() && reg.find("a.restart")->restartPending());
    // a prerequisite that is not auto-applied (a mode) is reported, not switched
    CHECK(reg.setText("a.root", "false") == Result::Ok);
    r = reg.setWithNeeds("a.manual", "true");
    CHECK(r.changes.empty() && !r.blocked.empty() && !reg.find("a.root")->asBool());
    // an unavailable prerequisite (no ray query here) is reported and stays off
    r = reg.setWithNeeds("a.needs_cap", "true");
    CHECK(r.changes.empty() && !r.blocked.empty() && !reg.find("a.capdep")->asBool());
    // a locked prerequisite is not touched
    reg.lock(*reg.find("a.root"), 0, "test");
    r = reg.setWithNeeds("a.mid", "high");
    CHECK(r.changes.empty() && !r.blocked.empty());
    // bad input
    CHECK(reg.setWithNeeds("a.nope", "1").result == Result::UnknownKey);
    CHECK(reg.setWithNeeds("a.mid", "sometimes").result == Result::BadValue);
    CHECK(reg.dependentsOf("a.mid").size() == 2);
    CHECK(Registry::needsText(mid.spec()) == "a.root=true\n");
}

TEST(settings_needs_of_the_renderer_settings) {
    using namespace rvk::settings;
    Registry reg;
    registerRendererSettings(reg);
    // every key a need names exists, its value parses, and there is no cycle through the registrations
    for (const Setting* s : reg.all()) {
        for (const Need& n : s->spec().needs) {
            const Setting* p = reg.find(n.key);
            double v = 0.0;
            CHECK_MSG(p != nullptr && Registry::parseText(p->spec(), n.value, v), s->key() + " needs " + n.key + "=" + n.value);
            CHECK(n.key != s->key());
        }
    }
    // the materials family acts only at "High": switching glass on switches the quality to High
    CHECK(reg.setText("materials.quality", "medium") == Result::Ok);
    const auto r = reg.setWithNeeds("materials.glass", "true");
    CHECK(r.result == Result::Ok);
    CHECK(!reg.needsReason(*reg.find("materials.glass")).empty() || reg.find("materials.quality")->asEnum() == "high");
}
