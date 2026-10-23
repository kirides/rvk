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
    // debug: the tint view is off by default and not part of A/B; draw records are on and hidden (cost measurements only)
    CHECK(r.tintDraws != nullptr && r.tintDraws->asEnum() == "off" && !r.tintDraws->abEligible());
    CHECK(r.drawRecords != nullptr && r.drawRecords->asBool() && (r.drawRecords->spec().flags & Flag::Hidden) != 0);
    // ... and so is the RT scene's source switch (diagnostics)
    CHECK(r.rtSceneViews != nullptr && r.rtSceneViews->asEnum() == "all" && (r.rtSceneViews->spec().flags & Flag::Hidden) != 0);
    // ... and the water's RT reflection tweaks (cost measurements)
    CHECK(r.rtReflectionTweaks != nullptr && r.rtReflectionTweaks->asInt() == 0 && (r.rtReflectionTweaks->spec().flags & Flag::Hidden) != 0);
    // ... and Vulkan's exclusive mode on top of exclusive fullscreen's display mode change (diagnostics); the fullscreen mode
    // itself is a player setting that needs the exclusive fullscreen capability
    CHECK(r.exclusiveAcquire != nullptr && r.exclusiveAcquire->asBool() && (r.exclusiveAcquire->spec().flags & Flag::Hidden) != 0);
    CHECK(r.fullscreen != nullptr && r.fullscreen->spec().defaultValue == 0 &&
          r.fullscreen->spec().requirements == rvk::caps::bit(rvk::caps::Cap::FullScreenExclusive));
    for (const Setting* s : reg.all()) {
        CHECK(s == r.drawRecords || s == r.rtSceneViews || s == r.rtReflectionTweaks || s == r.exclusiveAcquire || s == r.showAdvanced ||
              (s->spec().flags & Flag::Hidden) == 0);
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
