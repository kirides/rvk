#include "core/settings.h"

#include "core/config.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

namespace rvk::settings {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool validKey(std::string_view key) {
    if (key.empty() || key.front() == '.' || key.back() == '.' || key.find('.') == std::string_view::npos ||
        key.starts_with("ab.") || key.find("..") != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(key, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; });
}

// clamps / rounds / validates a value for a spec; false when it cannot be represented
bool normalise(const Spec& spec, double& v) {
    if (!std::isfinite(v)) return false;
    switch (spec.type) {
    case Type::Bool:
        v = v != 0.0 ? 1.0 : 0.0;
        return true;
    case Type::Int:
        v = std::clamp(std::round(v), spec.min, spec.max);
        return true;
    case Type::Float:
        v = std::clamp(v, spec.min, spec.max);
        return true;
    case Type::Enum: {
        const double i = std::round(v);
        if (i != v || i < 0.0 || i >= static_cast<double>(spec.options.size())) return false;
        return true;
    }
    }
    return false;
}

std::string tomlValue(const Spec& spec, double v) {
    switch (spec.type) {
    case Type::Bool: return v != 0.0 ? "true" : "false";
    case Type::Int: return std::format("{}", static_cast<std::int64_t>(v));
    case Type::Float: {
        std::string s = std::format("{}", v);
        if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // TOML floats need a '.' or an exponent
        return s;
    }
    case Type::Enum: return std::format("\"{}\"", spec.options[static_cast<std::size_t>(v)]);
    }
    return "?";
}

// a TOML node -> value of the spec
bool nodeValue(const Spec& spec, const toml::node& node, double& out) {
    if (const auto b = node.value_exact<bool>()) {
        if (spec.type != Type::Bool) return false;
        out = *b ? 1.0 : 0.0;
        return true;
    }
    if (const auto i = node.value_exact<std::int64_t>()) {
        // an Enum whose options are numbers (shadows.map_size = 2048): the option of that name, else the index
        if (spec.type == Type::Enum) {
            if (const auto it = std::ranges::find(spec.options, std::to_string(*i)); it != spec.options.end()) {
                out = static_cast<double>(it - spec.options.begin());
                return true;
            }
        }
        out = static_cast<double>(*i);
        return spec.type != Type::Bool || *i == 0 || *i == 1 ? normalise(spec, out) : false;
    }
    if (const auto d = node.value_exact<double>()) {
        if (spec.type != Type::Int && spec.type != Type::Float) return false;
        out = *d;
        return normalise(spec, out);
    }
    if (const auto s = node.value_exact<std::string>()) {
        return Registry::parseText(spec, *s, out);
    }
    return false;
}

void collectLeaves(const toml::table& t, const std::string& prefix, std::vector<std::string>& out) {
    for (const auto& [k, v] : t) {
        std::string path = prefix.empty() ? std::string(k.str()) : prefix + "." + std::string(k.str());
        if (const auto* sub = v.as_table()) {
            collectLeaves(*sub, path, out);
        } else {
            out.push_back(std::move(path));
        }
    }
}

}  // namespace

std::string_view toString(Result r) {
    switch (r) {
    case Result::Ok: return "ok";
    case Result::UnknownKey: return "unknown key";
    case Result::BadValue: return "bad value";
    case Result::Locked: return "locked by config.toml";
    }
    return "?";
}

std::string_view Setting::asEnum() const {
    const auto i = static_cast<std::size_t>(effective());
    return i < spec_.options.size() ? std::string_view(spec_.options[i]) : std::string_view();
}

// ---- registration ---------------------------------------------------------------------------------------------------

Setting& Registry::add(Spec spec) {
    const std::lock_guard lock(mutex_);
    for (auto& s : settings_) {
        if (s->spec_.key == spec.key) {
            spdlog::error("settings: '{}' registered twice", spec.key);
            return *s;
        }
    }
    double def = spec.defaultValue;
    if (!validKey(spec.key) || (spec.type == Type::Enum && spec.options.empty()) || !normalise(spec, def)) {
        spdlog::error("settings: malformed setting '{}' (key, enum options or default)", spec.key);
        spec.flags |= Flag::Hidden;
        spec.key = std::format("invalid.setting_{}", settings_.size());
        if (spec.type == Type::Enum && spec.options.empty()) spec.options = {"invalid"};
        def = 0.0;
    }
    spec.defaultValue = def;
    double fallback = spec.unmetValue.value_or(def);
    if (!normalise(spec, fallback)) fallback = def;
    auto s = std::unique_ptr<Setting>(new Setting(std::move(spec)));
    s->value_ = def;
    s->start_ = def;
    s->stored_ = def;
    s->other_ = def;
    s->fallback_ = fallback;
    if (s->spec_.requirements != 0 || !s->spec_.optionRequirements.empty()) {
        s->unmetReason_ = resolve(s->spec_, caps_);
        s->unmet_ = !s->unmetReason_.empty();
        resolveOptions(*s, caps_);
    }
    s->gated_ = (s->spec_.flags & Flag::Debug) != 0 && debugSwitch_ != nullptr && debugSwitch_->value() == 0.0;
    settings_.push_back(std::move(s));
    return *settings_.back();
}

Setting* Registry::find(std::string_view key) {
    const std::lock_guard lock(mutex_);
    for (auto& s : settings_) {
        if (s->spec_.key == key) return s.get();
    }
    return nullptr;
}

const Setting* Registry::find(std::string_view key) const { return const_cast<Registry*>(this)->find(key); }

std::vector<Setting*> Registry::all() const {
    const std::lock_guard lock(mutex_);
    std::vector<Setting*> out;
    out.reserve(settings_.size());
    for (const auto& s : settings_) out.push_back(s.get());
    return out;
}

std::size_t Registry::size() const {
    const std::lock_guard lock(mutex_);
    return settings_.size();
}

// ---- values ---------------------------------------------------------------------------------------------------------

void Registry::changedLocked(Setting& s) {
    s.pending_ = true;
    ++generation_;
    if (&s == debugSwitch_) gateLocked();
}

Result Registry::setLocked(Setting& s, double value) {
    if (!normalise(s.spec_, value)) return Result::BadValue;
    if (s.locked()) return Result::Locked;
    if (value != s.value()) {
        s.value_ = value;
        s.stored_ = value;
        changedLocked(s);
        dirty_ = true;
    }
    return Result::Ok;
}

Result Registry::set(Setting& s, double value) {
    const std::lock_guard lock(mutex_);
    return setLocked(s, value);
}

Result Registry::set(std::string_view key, double value) {
    Setting* s = find(key);
    return s != nullptr ? set(*s, value) : Result::UnknownKey;
}

bool Registry::parseText(const Spec& spec, std::string_view text, double& out) {
    text = trim(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
    if (spec.type == Type::Bool) {
        if (iequals(text, "true") || iequals(text, "on") || iequals(text, "yes") || text == "1") {
            out = 1.0;
            return true;
        }
        if (iequals(text, "false") || iequals(text, "off") || iequals(text, "no") || text == "0") {
            out = 0.0;
            return true;
        }
        return false;
    }
    if (spec.type == Type::Enum) {
        for (std::size_t i = 0; i < spec.options.size(); ++i) {
            if (iequals(text, spec.options[i])) {
                out = static_cast<double>(i);
                return true;
            }
        }
    }
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size() || text.empty()) return false;
    out = v;
    return normalise(spec, out);
}

std::string Registry::formatValue(const Spec& spec, double value) {
    if (spec.type == Type::Enum) {
        const auto i = static_cast<std::size_t>(value);
        return i < spec.options.size() ? spec.options[i] : std::string();
    }
    if (spec.type == Type::Float) return std::format("{}", value);
    return tomlValue(spec, value);
}

Result Registry::setText(std::string_view key, std::string_view text) {
    Setting* s = find(key);
    if (s == nullptr) return Result::UnknownKey;
    double v = 0.0;
    if (!parseText(s->spec_, text, v)) return Result::BadValue;
    return set(*s, v);
}

// ---- prerequisites (Spec::needs) ------------------------------------------------------------------------------------

namespace {
std::string optionText(const Spec& spec, double v) {
    if (spec.type == Type::Enum) {
        const auto i = static_cast<std::size_t>(v);
        if (i < spec.optionLabels.size()) return spec.optionLabels[i];
        if (i < spec.options.size()) return spec.options[i];
    }
    if (spec.type == Type::Bool) return v != 0.0 ? "on" : "off";
    return Registry::formatValue(spec, v);
}
}  // namespace

// a prerequisite is met while its effective value matches (or, negated, differs); `prereq` = the setting (null: unknown key = met)
bool Registry::needMet(const Need& n, const Setting*& prereq) const {
    prereq = find(n.key);
    if (prereq == nullptr) return true;
    double v = 0.0;
    if (!parseText(prereq->spec_, n.value, v)) return true;
    const bool eq = prereq->available() && prereq->effective() == v;
    return n.negate ? (prereq->available() && !eq) : eq;
}

// "on": a Bool true or an Enum option past "off" (settings without an off state never switch anything on)
bool Registry::needsOn(const Setting& s, double v) const {
    if (s.spec_.type == Type::Bool) return v != 0.0;
    if (s.spec_.type == Type::Enum && !s.spec_.options.empty() && (s.spec_.options[0] == "off" || s.spec_.options[0] == "none")) return v > 0.0;
    return false;
}

void Registry::applyNeeds(Setting& s, std::vector<Setting*>& visiting, NeedsResult& out) {
    if (std::ranges::find(visiting, &s) != visiting.end()) return;  // a cycle: cut
    visiting.push_back(&s);
    for (const Need& n : s.spec_.needs) {
        const Setting* cp = nullptr;
        if (needMet(n, cp)) continue;
        Setting* p = find(n.key);
        double want = 0.0;
        const auto blockWith = [&](const std::string& why) {
            if (!out.blocked.empty()) out.blocked += "; ";
            out.blocked += why;
        };
        if (!parseText(p->spec_, n.value, want)) continue;
        if (!n.autoApply) { blockWith(std::format("{} must be {}", p->spec_.label, optionText(p->spec_, want))); continue; }
        if (!p->available()) { blockWith(std::format("{}: {}", p->spec_.label, unavailableReason(*p))); continue; }
        if (p->locked()) { blockWith(std::format("{} is fixed by config.toml", p->spec_.label)); continue; }
        if (n.negate) {  // "not <value>": the nearest other option (Enum: the first past "off"), a Bool's opposite
            if (p->spec_.type == Type::Bool) {
                want = want != 0.0 ? 0.0 : 1.0;
            } else if (p->spec_.type == Type::Enum) {
                want = want == 0.0 ? 1.0 : 0.0;
            } else {
                continue;
            }
        }
        if (p->spec_.type == Type::Enum && !p->optionAvailable(want)) { blockWith(std::format("{} = {} is not available", p->spec_.label, optionText(p->spec_, want))); continue; }
        applyNeeds(*p, visiting, out);  // its own prerequisites first
        const double before = p->value();
        if (before == want) continue;
        const bool restart = p->restart() || (!p->spec_.restartGroups.empty() && p->restartGroup(before) != p->restartGroup(want));
        if (set(*p, want) == Result::Ok) out.changes.push_back({p->spec_.key, formatValue(p->spec_, before), formatValue(p->spec_, want), restart});
    }
    visiting.pop_back();
}

Registry::NeedsResult Registry::setWithNeeds(std::string_view key, std::string_view text) {
    NeedsResult out;
    Setting* s = find(key);
    if (s == nullptr) { out.result = Result::UnknownKey; return out; }
    double v = 0.0;
    if (!parseText(s->spec_, text, v)) { out.result = Result::BadValue; return out; }
    out.result = set(*s, v);
    if (out.result != Result::Ok || !needsOn(*s, v)) return out;
    std::vector<Setting*> visiting;
    applyNeeds(*s, visiting, out);
    return out;
}

std::string Registry::needsReasonDepth(const Setting& s, int depth) const {
    if (depth > 6) return {};
    std::string out;
    for (const Need& n : s.spec_.needs) {
        const Setting* p = nullptr;
        std::string why;
        if (!needMet(n, p)) {
            double v = 0.0;
            parseText(p->spec_, n.value, v);
            why = n.negate ? std::format("{} must not be {}", p->spec_.label, optionText(p->spec_, v))
                           : std::format("{} must be {}", p->spec_.label, optionText(p->spec_, v));
        } else if (p != nullptr) {
            why = needsReasonDepth(*p, depth + 1);  // met, but the prerequisite is itself greyed
        }
        if (why.empty()) continue;
        if (!out.empty()) out += "; ";
        out += why;
    }
    return out;
}

std::string Registry::needsReason(const Setting& s) const { return needsReasonDepth(s, 0); }

std::vector<std::string> Registry::dependentsOf(std::string_view key) const {
    std::vector<std::string> out;
    for (const auto& s : settings_) {
        if (std::ranges::any_of(s->spec_.needs, [&](const Need& n) { return n.key == key; })) out.push_back(s->spec_.key);
    }
    return out;
}

std::string Registry::needsText(const Spec& spec) {
    std::string out;
    for (const Need& n : spec.needs) out += std::format("{}{}{}\n", n.key, n.negate ? "!=" : "=", n.value);
    return out;
}

void Registry::lock(Setting& s, double value, std::string reason) {
    const std::lock_guard lock(mutex_);
    if (!normalise(s.spec_, value)) return;
    if (!s.locked()) s.stored_ = s.value();
    s.lockReason_ = std::move(reason);
    s.locked_ = true;
    if (value != s.value()) {
        s.value_ = value;
        changedLocked(s);
    }
}

std::string Registry::lockReason(const Setting& s) const {
    const std::lock_guard lock(mutex_);
    return s.lockReason_;
}

// ---- capability resolution ------------------------------------------------------------------------------------------

std::string Registry::resolve(const Spec& spec, const caps::Report& report) { return report.unmetReason(spec.requirements); }

void Registry::resolveOptions(Setting& s, const caps::Report& report) {
    std::uint32_t mask = 0;
    s.optionReasons_.assign(s.spec_.options.size(), std::string());
    for (std::size_t i = 0; i < s.spec_.optionRequirements.size() && i < s.spec_.options.size() && i < 32; ++i) {
        std::string reason = report.unmetReason(s.spec_.optionRequirements[i]);
        if (reason.empty()) continue;
        mask |= 1u << i;
        s.optionReasons_[i] = std::move(reason);
    }
    s.optionUnmet_ = mask;
}

std::string Registry::optionUnavailableReason(const Setting& s, std::size_t index) const {
    const std::lock_guard lock(mutex_);
    return index < s.optionReasons_.size() ? s.optionReasons_[index] : std::string();
}

void Registry::resolveLocked(Setting& s) {
    const double before = s.effective();
    s.unmetReason_ = resolve(s.spec_, caps_);
    s.unmet_ = !s.unmetReason_.empty();
    resolveOptions(s, caps_);
    if (s.effective() != before) changedLocked(s);
}

void Registry::setCapabilities(const caps::Report& report) {
    std::vector<std::string> notes;
    {
        const std::lock_guard lock(mutex_);
        caps_ = report;
        for (auto& s : settings_) {
            if (s->spec_.requirements == 0 && s->spec_.optionRequirements.empty()) continue;
            resolveLocked(*s);
            if (s->spec_.type == Type::Enum && s->available() && !s->optionAvailable(s->value())) {
                notes.push_back(std::format("settings: {} = {} requested, applies as {}: {}", s->spec_.key,
                                            formatValue(s->spec_, s->value()), formatValue(s->spec_, s->effective()),
                                            s->optionReasons_[static_cast<std::size_t>(s->value())]));
            }
            if (!s->available() && s->value() != s->fallback_) {
                notes.push_back(std::format("settings: {} = {} requested, applies as {}: {}", s->spec_.key,
                                            formatValue(s->spec_, s->value()), formatValue(s->spec_, s->fallback_),
                                            s->unmetReason_));
            }
        }
        ++generation_;
    }
    for (const auto& n : notes) spdlog::info("{}", n);
}

caps::Report Registry::capabilities() const {
    const std::lock_guard lock(mutex_);
    return caps_;
}

std::string Registry::unavailableReason(const Setting& s) const {
    const std::lock_guard lock(mutex_);
    return s.unmetReason_;
}

// ---- debug mode -----------------------------------------------------------------------------------------------------

void Registry::gateLocked() {
    const bool on = debugSwitch_ == nullptr || debugSwitch_->value() != 0.0;
    for (auto& s : settings_) {
        if ((s->spec_.flags & Flag::Debug) == 0) continue;
        const double before = s->effective();
        s->gated_ = !on;
        if (s->effective() != before) changedLocked(*s);
    }
    // the A/B comparison is part of debug mode
    if (!on && bActive()) flipLocked();
    ++generation_;
}

void Registry::setDebugSwitch(Setting& s) {
    const std::lock_guard lock(mutex_);
    debugSwitch_ = &s;
    gateLocked();
}

bool Registry::debugMode() const {
    const std::lock_guard lock(mutex_);
    return debugSwitch_ == nullptr || debugSwitch_->value() != 0.0;
}

// ---- callbacks ------------------------------------------------------------------------------------------------------

void Registry::onChange(Setting& s, std::function<void(const Setting&)> fn, const void* owner) {
    const std::lock_guard lock(mutex_);
    s.callbacks_.push_back({owner, std::move(fn)});
}

void Registry::removeCallbacks(const void* owner) {
    const std::lock_guard lock(mutex_);
    for (auto& s : settings_) {
        std::erase_if(s->callbacks_, [&](const Setting::Callback& c) { return c.owner == owner; });
    }
}

std::size_t Registry::dispatch() {
    std::vector<std::pair<Setting*, std::vector<Setting::Callback>>> work;
    {
        const std::lock_guard lock(mutex_);
        for (auto& s : settings_) {
            if (s->pending_) {
                s->pending_ = false;
                work.emplace_back(s.get(), s->callbacks_);
            }
        }
    }
    for (auto& [s, fns] : work) {
        for (auto& c : fns) c.fn(*s);
    }
    return work.size();
}

// ---- A/B ------------------------------------------------------------------------------------------------------------

void Registry::flipAB() {
    const std::lock_guard lock(mutex_);
    flipLocked();
}

void Registry::flipLocked() {
    for (auto& s : settings_) {
        if (!s->abEligible()) continue;
        const double v = s->value();
        if (s->other_ != v) {
            s->value_ = s->other_;
            s->stored_ = s->other_;
            s->other_ = v;
            changedLocked(*s);
        }
    }
    bActive_ = !bActive_;
    ++generation_;
}

void Registry::copyActiveToOther() {
    const std::lock_guard lock(mutex_);
    for (auto& s : settings_) {
        if (s->abEligible() && s->other_ != s->value()) {
            s->other_ = s->value();
            dirty_ = true;
        }
    }
    ++generation_;
}

void Registry::resetOther() {
    const std::lock_guard lock(mutex_);
    for (auto& s : settings_) {
        if (s->other_ != s->spec_.defaultValue) {
            s->other_ = s->spec_.defaultValue;
            dirty_ = true;
        }
    }
    ++generation_;
}

double Registry::otherValue(const Setting& s) const {
    const std::lock_guard lock(mutex_);
    return s.other_;
}

std::vector<const Setting*> Registry::abDifferences() const {
    const std::lock_guard lock(mutex_);
    std::vector<const Setting*> out;
    for (const auto& s : settings_) {
        if (s->abEligible() && s->other_ != s->value()) out.push_back(s.get());
    }
    return out;
}

// ---- persistence ----------------------------------------------------------------------------------------------------

std::string Registry::toToml() const {
    const std::lock_guard lock(mutex_);
    // side A = the active values unless B is active (then A sits in other_)
    const bool b = bActive();
    struct Entry {
        const Setting* s;
        double v;
    };
    // table -> entries, tables in order of first appearance
    std::vector<std::pair<std::string, std::vector<Entry>>> tables;
    const auto put = [&](const std::string& table, const Setting* s, double v) {
        auto it = std::ranges::find_if(tables, [&](const auto& t) { return t.first == table; });
        if (it == tables.end()) {
            tables.emplace_back(table, std::vector<Entry>{});
            it = tables.end() - 1;
        }
        it->second.push_back({s, v});
    };
    for (int side = 0; side < 2; ++side) {
        for (const auto& s : settings_) {
            const bool eligible = s->abEligible();
            double v = 0.0;
            if (side == 0) {
                v = s->locked() ? s->stored_ : (b && eligible ? s->other_ : s->value());
            } else {
                if (!eligible) continue;
                v = b ? s->value() : s->other_;
            }
            if (v == s->spec_.defaultValue) continue;
            const std::string& key = s->spec_.key;
            const auto dot = key.rfind('.');
            put((side == 1 ? "ab." : "") + key.substr(0, dot), s.get(), v);
        }
    }
    std::string out =
        "# Risen Enhanced Graphics: player settings. The in-game overlay (F11 by default) writes this file.\n"
        "# Only values that differ from the defaults are listed; delete a line (or the file) to go back to the default.\n"
        "# [ab.*] tables hold side B of the A/B comparison (missing values = defaults).\n";
    for (const auto& [table, entries] : tables) {
        out += std::format("\n[{}]\n", table);
        for (const auto& e : entries) {
            const std::string& key = e.s->spec_.key;
            out += std::format("{} = {}\n", key.substr(key.rfind('.') + 1), tomlValue(e.s->spec_, e.v));
        }
    }
    return out;
}

namespace {
// settings of earlier builds that were taken out (the early native prototypes: native-renderer.md N3 / N4; the settings clean-up of
// docs/research/settings-audit.md: finished A/B switches and art coefficients, now constants in the code); an old settings.toml /
// config.toml that still has them loads, with one warning per key, and the next save leaves them out
constexpr std::string_view kRemovedSettings[] = {"native.opaque", "native.shadows", "native.debug", "rt.source",
    "ao.radius", "debug.constant_compare", "debug.draw_records", "debug.fast_absorb", "debug.fast_sort", "debug.fs_read_cache",
    "debug.light_collect", "debug.ocean_spd", "debug.pace_loading", "debug.sky_cube_interval", "debug.sound_compressed",
    "debug.split_constants", "debug.xinput_interval", "gi.bleed", "gi.characters", "gi.cutouts", "gi.directional", "gi.emissive",
    "gi.light_count", "gi.lights", "gi.shadow_fill", "lighting.light_shadow_baked", "lighting.light_shadow_radius",
    "lighting.table_complete", "materials.puddles", "lighting.tree_merge", "lighting.trust_table", "materials.default_roughness",
    "materials.derive_max", "materials.derive_min", "materials.environment", "materials.rt_strength", "materials.specular",
    "materials.wetness", "native.cpu_opts", "native.gpu_scene", "native.lean_effects", "native.lean_restore", "native.mirror",
    "native.retained", "native.retained_check", "native.retained_groups", "rt.lod_bias", "rt.lod_far_range", "rt.lod_range",
    "shadows.cascade_blend", "shadows.fade", "shadows.rt_softness", "shadows.rt_wind", "shadows.softness", "shadows.split",
    "upscale.post_full_size", "upscale.reactive_cutoff", "upscale.reactive_scale", "water.murkiness",
    "water.reflection_strength",};
}  // namespace

bool Registry::fromToml(std::string_view text, std::vector<std::string>* warnings) {
    const auto warn = [&](std::string w) {
        if (warnings != nullptr) warnings->push_back(std::move(w));
    };
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        warn(std::format("{} (line {})", e.description(), e.source().begin.line));
        return false;
    }
    std::vector<std::string> leaves;
    collectLeaves(tbl, "", leaves);
    const std::lock_guard lock(mutex_);
    const bool b = bActive();
    std::vector<std::string> removedSeen;
    for (const auto& leaf : leaves) {
        const bool sideB = leaf.starts_with("ab.");
        std::string key = sideB ? leaf.substr(3) : leaf;
        Setting* s = nullptr;
        for (auto& x : settings_) {
            if (x->spec_.key == key) s = x.get();
        }
        if (s == nullptr) {
            // a setting of an earlier build that no longer exists: one line per key (also its [ab.*] twin), dropped at the next save
            if (std::ranges::find(kRemovedSettings, key) != std::end(kRemovedSettings)) {
                if (std::ranges::find(removedSeen, key) == removedSeen.end()) {
                    removedSeen.push_back(key);
                    warn(std::format("'{}' was removed, ignoring", key));
                }
            } else {
                warn(std::format("unknown setting '{}' ignored", leaf));
            }
            continue;
        }
        double v = 0.0;
        const toml::node* node = tbl.at_path(leaf).node();
        if (node == nullptr || !nodeValue(s->spec_, *node, v)) {
            warn(std::format("{}: bad value, keeping {}", leaf, formatValue(s->spec_, sideB ? s->other_ : s->value())));
            continue;
        }
        if (sideB) {
            if (!s->abEligible()) {
                warn(std::format("{}: not part of the A/B comparison, ignored", leaf));
                continue;
            }
            // side B is the active one only if a flip happened before loading (not at startup)
            if (b) {
                if (s->value() != v) {
                    s->value_ = v;
                    s->stored_ = v;
                    changedLocked(*s);
                }
            } else {
                s->other_ = v;
            }
            continue;
        }
        if (s->locked()) {
            s->stored_ = v;
            continue;
        }
        if (b && s->abEligible()) {
            s->other_ = v;
        } else if (s->value() != v) {
            s->value_ = v;
            s->stored_ = v;
            changedLocked(*s);
        }
    }
    ++generation_;
    return true;
}

bool Registry::load(const std::filesystem::path& file, std::vector<std::string>* warnings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return true;
    std::ostringstream ss;
    ss << in.rdbuf();
    const bool ok = fromToml(ss.str(), warnings);
    dirty_ = false;
    return ok;
}

bool Registry::save(const std::filesystem::path& file, std::string* error) {
    const std::string text = toToml();
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            if (error != nullptr) *error = "cannot write " + tmp.string();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        if (error != nullptr) *error = std::format("cannot rename the temporary file (error {})", GetLastError());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    dirty_ = false;
    return true;
}

void Registry::markStarted() {
    const std::lock_guard lock(mutex_);
    for (auto& s : settings_) s->start_ = s->value();
}

Registry& registry() {
    static Registry r;
    return r;
}

// ---- the renderer's settings ----------------------------------------------------------------------------------------

namespace {
RendererSettings g_renderer;
}

const std::vector<std::string>& hotkeyNames() {
    static const std::vector<std::string> names = {"none", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11",
                                                   "f12", "insert", "home", "end", "page_up", "page_down", "pause", "scroll_lock"};
    return names;
}

RendererSettings registerRendererSettings(Registry& reg) {
    const auto keyIndex = [](std::string_view name) {
        const auto& n = hotkeyNames();
        return static_cast<double>(std::ranges::find(n, name) - n.begin());
    };
    std::vector<std::string> keyLabels;
    for (const auto& n : hotkeyNames()) {
        std::string l = n == "none" ? "(none)" : n;
        for (auto& c : l) c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        keyLabels.push_back(std::move(l));
    }
    // Registration order = the overlay's order: tabs, then sections by first appearance. Descriptions are the overlay's
    // tooltips: player language, except for the Debug tab.
    RendererSettings r;

    // ---- Display tab ----
    r.vsync = &reg.add({
        .key = "vulkan.vsync",
        .label = "Vertical sync",
        .category = "Display",
        .tab = kTabDisplay,
        .description = "Waits for the monitor before showing each frame: no tearing, but the frame rate cannot go above the "
                       "monitor's refresh rate. \"Game option\" uses the setting from the game's own video options.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"engine", "on", "off"},
        .optionLabels = {"Game option", "On", "Off"},
        .page = "Display", .section = "Display",
    });
    r.maxFps = &reg.add({
        .key = "vulkan.max_fps",
        .label = "Frame rate limit",
        .category = "Display",
        .tab = kTabDisplay,
        .description = "The highest frame rate the game runs at. A limit saves power and keeps the graphics card cooler. With "
                       "vertical sync on, a limit below the monitor's refresh rate keeps frames in step with the monitor "
                       "(e.g. 60 on a 120 Hz monitor). With frame generation on, this is the limit of the frames shown: the game "
                       "renders half as many (120 = 60 rendered). Loading screens are never limited, so loading stays fast.",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 500,
        .format = "%.0f fps",
        .autoLabel = "Unlimited",
        .page = "Display", .section = "Display",
    });
    r.menuFps = &reg.add({
        .key = "vulkan.menu_fps",
        .label = "Menu frame rate limit",
        .category = "Display",
        .tab = kTabDisplay,
        .description = "The highest frame rate while a menu is open (the game menu, inventory, character screen, map, ...), so "
                       "the graphics card does not work flat out on a still picture. Loading screens are never limited, so "
                       "loading stays fast.",
        .type = Type::Float,
        .defaultValue = 60,
        .min = 0,
        .max = 240,
        .format = "%.0f fps",
        .autoLabel = "Unlimited",
        .page = "Display", .section = "Display",
    });
    r.fullscreen = &reg.add({
        .key = "vulkan.fullscreen",
        .label = "Fullscreen mode",
        .category = "Display",
        .tab = kTabDisplay,
        .description = "How the game's fullscreen setting is shown. Borderless window: covers the monitor at the desktop's "
                       "resolution, switching to other programs is instant. Exclusive: the monitor switches to the resolution "
                       "and refresh rate chosen in the game's video options, and back when you switch away.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"borderless", "exclusive"},
        .optionLabels = {"Borderless window", "Exclusive"},
        .flags = Flag::NoAB,
        .requirements = caps::bit(caps::Cap::FullScreenExclusive),
        .page = "Display", .section = "Display",
    });

    // ---- System tab ----
    // the render mode (core/config.h RenderMode; docs/research/overlay.md 3): settings.toml is its source of truth,
    // config.toml's [renderer] mode locks it when not the default (bootstrap). Wrapper <-> Full apply at the next frame
    // boundary (HalDevice::applyStaticOps); Original runs on another backend: after a restart (restartGroups).
    r.rendererMode = &reg.add({
        .key = "renderer.mode",
        .label = "Render mode",
        .category = "Renderer",
        .tab = kTabSystem,
        .description = "Wrapper: the game's own rendering translated to Vulkan, with the enhancements (recommended). Full: the "
                       "renderer draws the whole frame itself from the game's data (experimental; costs some processor time "
                       "for now). Original: the game's own Direct3D 9 renderer, without Vulkan, enhancements or this menu; "
                       "it applies after a restart. To come back from Original, delete the [renderer] lines in "
                       "rvk\\settings.toml.",
        .type = Type::Enum,
        .defaultValue = static_cast<double>(kDefaultRenderMode),
        .options = {"original", "wrapper", "full"},
        .optionLabels = {"Original (Direct3D 9)", "Wrapper (Vulkan)", "Full (Vulkan, experimental)"},
        .restartGroups = {0, 1, 1},
        .page = "Display", .section = "Render mode", .order = -10,
    });
    r.toggleKey = &reg.add({
        .key = "overlay.toggle_key",
        .label = "Settings menu key",
        .category = "Menu",
        .tab = kTabSystem,
        .description = "The key that opens and closes this menu. The game never receives it.",
        .type = Type::Enum,
        .defaultValue = keyIndex("f11"),
        .options = hotkeyNames(),
        .optionLabels = keyLabels,
        .flags = Flag::NoAB,
        .page = "System",
    });
    r.fpsCounter = &reg.add({
        .key = "overlay.fps_counter",
        .label = "Show frame rate",
        .category = "Menu",
        .tab = kTabSystem,
        .description = "Shows the frame rate in the top left corner while this menu is closed.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB,
        .page = "System",
    });
    r.debugMode = &reg.add({
        .key = "overlay.debug_mode",
        .label = "Debug mode",
        .category = "Menu",
        .tab = kTabSystem,
        .description = "Developer tools: debug views, an A/B settings comparison and technical details. Not needed for "
                       "playing. Turning it off also turns the debug views off.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Advanced,
        .page = "System",
    });
    r.showAdvanced = &reg.add({
        .key = "overlay.show_advanced",
        .label = "Show advanced settings",
        .category = "Menu",
        .tab = kTabSystem,
        .description = "Also shows the fine-tuning settings (wave height, shadow distance, ...). Always on in debug mode.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Hidden,  // the checkbox at the top of the menu
        .page = "System",
    });
    r.api = &reg.add({
        .key = "vulkan.api",
        .label = "Graphics API",
        .category = "Hardware",
        .tab = kTabSystem,
        .description = "Which graphics API the renderer uses. Vulkan is the default and the tested one. Direct3D 12 is experimental: it needs "
                       "Shader Model 6.6 and falls back to Vulkan when the card or driver cannot do it. Frame generation and FSR upscaling "
                       "(Image) and exclusive fullscreen are Vulkan only for now.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"vulkan", "d3d12"},
        .optionLabels = {"Vulkan", "Direct3D 12 (experimental)"},
        .flags = Flag::Restart,  // the D3D12 backend is not released yet: the overlay shows it in debug mode only (View::visible), device.cpp ignores it otherwise
        .page = "Display",
        .section = "Render mode",
        .order = -5,
    });
    r.adapter = &reg.add({
        .key = "vulkan.adapter",
        .label = "Graphics card number",
        .category = "Hardware",
        .tab = kTabSystem,
        .description = "Which graphics card renders the game when the PC has more than one. Auto picks the fastest dedicated "
                       "card. The numbered list is in rvk\\rvk.log (\"probe\").",
        .type = Type::Int,
        .defaultValue = -1,
        .min = -1,
        .max = 15,
        .autoLabel = "Auto",
        .flags = Flag::Restart | Flag::Advanced,
        .page = "System",
    });
    r.lowBandwidth = &reg.add({
        .key = "vulkan.low_bandwidth",
        .label = "Low bandwidth mode",
        .category = "Hardware",
        .tab = kTabSystem,
        .description = "Saves memory bandwidth on graphics cards that share the PC's memory (integrated graphics, e.g. a Radeon "
                       "680M) by drawing a few of the enhancements' own intermediate images smaller or with less precision: "
                       "ambient occlusion, the global illumination probes and traced reflections. A little less "
                       "detail in those effects, the game's own picture is unchanged. Auto turns it on for integrated graphics "
                       "only. Takes effect at once.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"auto", "off", "on"},
        .optionLabels = {"Auto", "Off", "On"},
        .page = "System",
        .section = "Hardware",
    });
    r.exclusiveAcquire = &reg.add({
        .key = "vulkan.exclusive_acquire",
        .label = "Vulkan exclusive mode",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Exclusive fullscreen: also take Vulkan's exclusive mode (VK_EXT_full_screen_exclusive) on top of the "
                       "display mode change. Diagnostics (docs/research/window-mode.md 8.3).",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Hidden | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.rtAllow = &reg.add({
        .key = "rt.allow",
        .label = "Ray tracing support",
        .category = "Hardware",
        .tab = kTabSystem,
        .description = "Turns on the graphics card's ray tracing when the game starts; the ray-traced effects need it. Turn "
                       "it off only if the game has problems with your graphics driver. Greyed out when the card has no ray "
                       "tracing.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Restart | Flag::Advanced,
        .requirements = caps::bit(caps::Cap::RayTracingHardware),
        .unmetValue = 0,
        .page = "System",
    });
    r.igpuPresetDone = &reg.add({
        .key = "system.igpu_preset_done",
        .label = "Integrated graphics defaults applied",
        .category = "Hardware",
        .tab = kTabSystem,
        .description = "Set once the first run on an integrated graphics card has picked cheaper defaults (the Integrated "
                       "graphics preset) for the settings you had not changed.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Hidden,
        .page = "System",
        .section = "Hardware",
    });

    r.highFps = &reg.add({
        .key = "compat.high_fps",
        .label = "High Fps compatibility (Vaana)",
        .category = "Gameplay",
        .tab = kTabSystem,
        .description = "Fixes for gameplay bugs at high frame rates: the hero stopping in mid-air during long falls, "
                       "climbing at over 120 fps, and far too much thunder in storms. Ported from VaanaCZ's Risen FPS Patch "
                       "(github.com/VaanaCZ/Risen-FPS-Patch). Off: the game's own behaviour.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB,
        .page = "System",
    });

    // ---- Debug tab (debug mode) ----
    r.abKey = &reg.add({
        .key = "overlay.ab_key",
        .label = "A/B flip key",
        .category = "A/B options",
        .tab = kTabDebug,
        .description = "Hotkey that flips between settings side A and side B (also while the menu is closed). The game never "
                       "sees it.",
        .type = Type::Enum,
        .defaultValue = keyIndex("f10"),
        .options = hotkeyNames(),
        .optionLabels = keyLabels,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "A/B options",
    });
    r.abBadge = &reg.add({
        .key = "overlay.ab_badge",
        .label = "A/B indicator",
        .category = "A/B options",
        .tab = kTabDebug,
        .description = "Show which side (A or B) is active after a flip, and a small \"B\" while side B stays active.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "A/B options",
    });
    r.tintDraws = &reg.add({
        .key = "debug.tint_draws",
        .label = "Tint draws",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view: colours the scene's draws by what they are (kind: water blue, SpeedTree green, "
                       "skinned red, static meshes grey, instanced cyan, particles orange, unknown magenta, ...) or by their "
                       "pass (solid grey, refracted blue, alpha orange). Off renders exactly as without it.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "kind", "pass"},
        .optionLabels = {"Off", "By kind", "By pass"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.debugCrashMarkers = &reg.add({
        .key = "debug.crash_markers",
        .label = "Crash markers",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "After a GPU hang / device loss the log names the regions (views, passes, injected passes) the GPU had not "
                       "finished (AMD buffer markers around every capture region; needs Capture regions). Costs a few GPU writes "
                       "per region.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.debugRegions = &reg.add({
        .key = "debug.regions",
        .label = "Capture regions",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Names the frame's parts (views, passes, the renderer's own passes) for graphics captures (RenderDoc, PIX, "
                       "Radeon GPU Profiler) with debug labels: a capture shows a tree instead of one long list of draws.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.debugNames = &reg.add({
        .key = "debug.names",
        .label = "Capture resource names",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Names the renderer's images, buffers, shaders and pipelines (shadow maps, depth map, scene image, "
                       "refraction copy, ...) for graphics captures (RenderDoc's resource inspector, validation messages). "
                       "Objects are named when they are created: takes effect at the next start.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Debug | Flag::Restart,
        .page = "Debug", .section = "Renderer internals",
    });
    r.recordThreads = &reg.add({
        .key = "debug.record_threads",
        .label = "Record draws on worker threads",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Records the opaque passes' draws into secondary command buffers on two worker threads while the engine "
                       "thread goes on (less CPU time on the engine thread). Off = recorded inline, to compare.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.ubershaderSkip = &reg.add({
        .key = "debug.ubershader_skip",
        .label = "HLSL material shaders: keep classes translated",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Bit mask of material shader classes that stay on the translated shaders (1 z pass, 2 masked, 4 alpha blend, "
                       "8 lightmap, 16 normal map, 32 specular, 64 distortion, 128 plain, 256 the vertex shaders): to find which kind differs.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 511,
        .flags = Flag::Debug | Flag::NoAB,
        .page = "Debug", .section = "Renderer internals",
    });
    r.ubershader = &reg.add({
        .key = "debug.ubershader",
        .label = "HLSL material shaders",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Draws the game's material pixel shaders with the HLSL port of its material shader (built from the same "
                       "macros the game compiled them with) instead of the translated precompiled shaders. Shaders the port "
                       "cannot build stay translated.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.materialSources = &reg.add({
        .key = "debug.material_sources",
        .label = "Collect material shader sources",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Records, for every material shader the game asks for, its effect, entry function and the full macro list "
                       "(the material graph's code snippets included) and writes them to bin/rvk/cache/materials/sources.jsonl "
                       "(developer data for the HLSL material shader).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug | Flag::NoAB,
        .page = "Debug", .section = "Renderer internals",
    });
    r.reverseZ = &reg.add({
        .key = "debug.reverse_z",
        .label = "Reversed infinite depth",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The main view's depth buffer holds near / distance (1 at the near plane, 0 at infinity) with a floating point "
                       "buffer instead of the game's 24 bit depth: no z-fighting far away, no far clipping. Applies from the next "
                       "frame (the native passes read it when they are built).",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Debug | Flag::NoAB,
        .page = "Debug", .section = "Renderer internals",
    });
    r.batchDraws = &reg.add({
        .key = "debug.batch_draws",
        .label = "Batch opaque draws",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Records the opaque passes' mesh draws grouped by pipeline (fewer pipeline switches, less CPU time per "
                       "draw). Off = every draw in the engine's order, to compare.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.splitVerify = &reg.add({
        .key = "debug.split_verify",
        .label = "Verify split constants",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Checks, for every draw with split constants, that the window blocks its shaders read hold the engine's "
                       "register values (a mismatch is logged and counted; costs CPU time).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.injectView = &reg.add({
        .key = "debug.inject_view",
        .label = "Injection test view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view that tests the enhancement plumbing (a GLSL compute pass injected after the opaque "
                       "scene): \"Depth\" colours the scene by distance from the engine's depth map and draws a 5 m grid "
                       "fixed in the world, \"Tint\" cycles the scene's colour over time. Off renders exactly as without it.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "depth", "tint"},
        .optionLabels = {"Off", "Depth + world grid", "Tint"},
        .flags = Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.rtDebug = &reg.add({
        .key = "rt.debug",
        .label = "Ray tracing scene view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the ray tracing scene (the world geometry ray-traced effects see): \"Instances\" colours "
                       "each traced object, \"Depth check\" compares the traced distance with the rendered one (green = match, "
                       "red = missing in the RT scene, magenta = extra / misplaced, blue = missed), \"Normals\" shows the traced "
                       "surface normals, \"Albedo\" the traced surfaces' textures (unlit; split: the right half only, opaque: "
                       "cut-outs solid), \"UV\" their texture coordinates. Needs ray tracing. Off renders exactly as without it.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "instances", "depth", "normals", "albedo", "albedo_split", "albedo_opaque", "uv", "roughness", "metalness"},
        .optionLabels = {"Off", "Instances", "Depth check", "Normals", "Albedo", "Albedo (split)", "Albedo (cut-outs solid)", "UV", "Roughness (pack)", "Metalness (pack)"},
        .flags = Flag::NoAB | Flag::Debug,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .page = "Debug", .section = "Debug views",
    });
    r.rtReflectionTweaks = &reg.add({
        .key = "rt.reflection_tweaks",
        .label = "RT reflection tweaks",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer switch (cost measurements of rt.reflections): 1 cut-outs solid, 2 no shadow rays, 4 no "
                       "screen-space fallback, 8 hits unshaded.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 15,
        .flags = Flag::NoAB | Flag::Hidden | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.rtSceneViews = &reg.add({
        .key = "rt.scene_views",
        .label = "RT scene sources",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer switch: which views feed the ray tracing scene: the main view and the sun's shadow cascades "
                       "(all), only the main view, or only the cascades.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"all", "main", "shadow"},
        .optionLabels = {"Main view + sun cascades", "Main view", "Sun cascades"},
        .flags = Flag::NoAB | Flag::Hidden | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });

    r.rtTableEntries = &reg.add({
        .key = "rt.table_entries",
        .label = "RT scene: persistent entries",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only (frame-ownership.md F8-0, no picture change intended): the ray tracing scene learns each static, non-instanced, opaque mesh draw once "
                       "and then holds it as a persistent entry keyed by its scene-table slot (entity, op, draw range): while the entity is cached in and unmoved the draw "
                       "is no longer shown to the scene's observer (no query, no sighting), its instance stays in the TLAS. Every 32nd frame per entry the draw is observed "
                       "again (materials). rvk.log `rt: scene table (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"renderer.mode", "full", false, false}},
        .page = "Debug", .section = "Native frame",
    });
    r.rtTableCompare = &reg.add({
        .key = "debug.rt_table_compare",
        .label = "RT scene: compare the persistent entries",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "With rt.table_entries: every 10th frame the pre-filter is off, every covered draw is observed in full and diffed against its entry (BLAS key, rows, "
                       "flags, material); any DIFFERING in a 5 s window turns the entries off (rvk.log `rt: scene table (5 s)`).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"rt.table_entries", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });

    // ---- Graphics tab: water ----
    r.waterEnabled = &reg.add({
        .key = "water.enabled",
        .label = "Enhanced water",
        .category = "Water",
        .description = "New water: clear shallows that get darker with depth, reflections of the world and the sky, sun "
                       "glints and foam. Every lake, river and swamp keeps its original colour. Off: the game's original "
                       "water.",
        .type = Type::Bool,
        .defaultValue = 1,
        .page = "Water & Materials", .section = "Water",
    });
    r.waterQuality = &reg.add({
        .key = "water.quality",
        .label = "Water quality",
        .category = "Water",
        .description = "Detail of the water's reflections and of the sea's waves. Lower it if the frame rate drops near water.",
        .type = Type::Enum,
        .defaultValue = 2,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .needs = {{"water.enabled", "true", false, true}},
        .page = "Water & Materials", .section = "Water",
    });
    r.rtReflections = &reg.add({
        .key = "rt.reflections",
        .label = "Ray-traced water reflections",
        .category = "Ray tracing",
        .description = "Reflections on the water traced through the world: cliffs, trees, ships and houses are reflected even "
                       "when they are not on screen, with their own textures and shadows. Low is the fastest, High looks "
                       "best. Off: the water reflects only what is on screen. Needs a graphics card with ray tracing.",
        .type = Type::Enum,
        .defaultValue = 3,
        .options = {"off", "low", "medium", "high"},
        .optionLabels = {"Off", "Low", "Medium", "High"},
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .needs = {{"water.enabled", "true", false, true}, {"water.reflections", "ssr", false, true}},
        .page = "Water & Materials", .section = "Water",
    });
    r.waterOcean = &reg.add({
        .key = "water.ocean",
        .label = "Sea waves",
        .category = "Water",
        .description = "Waves on the open sea. Realistic: a simulated sea with big waves far out, smaller ones near the shore "
                       "and in the harbour, breaking waves and foam. Simple: a few rolling waves, faster. Off: the game's flat "
                       "sea. Lakes, rivers and swamps never get waves.",
        .type = Type::Enum,
        .defaultValue = 2,
        .options = {"off", "gerstner", "fft"},
        .optionLabels = {"Off", "Simple", "Realistic"},
        .needs = {{"water.enabled", "true", false, true}},
        .page = "Water & Materials", .section = "Sea",
    });
    r.waterOceanQuality = &reg.add({
        .key = "water.ocean_quality",
        .label = "Sea detail",
        .category = "Water",
        .description = "Detail of the realistic sea's waves. Lower it if the frame rate drops at the coast.",
        .type = Type::Enum,
        .defaultValue = 2,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .flags = Flag::Advanced,
        .needs = {{"water.ocean", "fft", false, true}},
        .page = "Water & Materials", .section = "Sea",
    });
    r.waterSeaState = &reg.add({
        .key = "water.sea_state",
        .label = "Sea state",
        .category = "Water",
        .description = "How rough the sea is. Follow weather: calm in sunshine, rough in rain, high waves in storms.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"auto", "calm", "moderate", "rough", "storm"},
        .optionLabels = {"Follow weather", "Calm", "Moderate", "Rough", "Storm"},
        .needs = {{"water.ocean", "off", true, true}},
        .page = "Water & Materials", .section = "Sea",
    });
    r.waterReflections = &reg.add({
        .key = "water.reflections",
        .label = "Water reflections",
        .category = "Water",
        .description = "World: the water reflects the shores, trees and ships (needed for ray-traced reflections). Sky only: "
                       "just the sky, faster.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"sky", "ssr"},
        .optionLabels = {"Sky only", "World"},
        .flags = Flag::Advanced,
        .needs = {{"water.enabled", "true", false, true}},
        .page = "Water & Materials", .section = "Water",
    });
    r.waterWindDirection = &reg.add({
        .key = "water.wind_direction",
        .label = "Wind direction",
        .category = "Water",
        .description = "The direction the wind pushes the waves, in degrees. Auto: the island's usual wind.",
        .type = Type::Int,
        .defaultValue = -1,
        .min = -1,
        .max = 359,
        .format = "%d degrees",
        .autoLabel = "Auto",
        .flags = Flag::Advanced,
        .needs = {{"water.ocean", "off", true, true}},
        .page = "Water & Materials", .section = "Sea",
    });
    r.waterWaveScale = &reg.add({
        .key = "water.wave_scale",
        .label = "Wave height",
        .category = "Water",
        .description = "Makes the sea's waves higher or lower (1 = natural for the sea state).",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 3,
        .format = "%.2f",
        .flags = Flag::Advanced,
        .needs = {{"water.ocean", "off", true, true}},
        .page = "Water & Materials", .section = "Sea",
    });
    r.waterDebug = &reg.add({
        .key = "water.debug",
        .label = "Water debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the enhanced water: the shore field (distance to the coast), the water depth, the "
                       "surface normals, the reflection alone, the foam (red) over the surf zone (green) and the depth (blue; FFT ocean: "
                       "red folding foam, green breaking waves, blue surf, white contact), the sea floor field "
                       "(red depth, green exposure, blue the waves' phase rolling in) or the FFT ocean's attenuation (red / "
                       "green / blue the three longest cascades' weights), or where each reflection comes from (rt.reflections: green the "
                       "screen, blue a shaded ray-traced hit, yellow the screen-space fallback, black the sky; brightness = "
                       "coverage); \"Reflection (RT hits all shaded)\" = the reflection with every ray-traced hit shaded, none "
                       "taken from the screen (the exposure match of the two).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "shore_field", "depth", "normals", "reflection", "foam", "sea_floor", "waves", "reflection_source", "reflection_shaded"},
        .optionLabels = {"Off", "Shore field", "Depth", "Normals", "Reflection", "Foam", "Sea floor", "Wave attenuation", "Reflection source", "Reflection (RT hits all shaded)"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });

    // ---- Graphics tab: shadows ----
    r.shadowsEnabled = &reg.add({
        .key = "shadows.enabled",
        .label = "Enhanced shadows",
        .category = "Shadows",
        .description = "Smoother sun shadows: soft edges instead of grainy ones, no visible line where the shadow detail "
                       "changes, and shadows that fade in gently in the distance instead of popping up. Off: the game's "
                       "original shadows.",
        .type = Type::Bool,
        .defaultValue = 1,
        .page = "Lighting & Shadows", .section = "Sun shadows",
    });
    r.shadowsFilter = &reg.add({
        .key = "shadows.filter",
        .label = "Shadow edge quality",
        .category = "Shadows",
        .description = "How smooth the shadow edges are. Higher costs more, mostly at high screen resolutions.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .needs = {{"shadows.enabled", "true", false, true}},
        .page = "Lighting & Shadows", .section = "Sun shadows",
    });
    r.shadowsMapSize = &reg.add({
        .key = "shadows.map_size",
        .label = "Shadow resolution",
        .category = "Shadows",
        .description = "How sharp sun shadows are. Higher is sharper and uses more graphics memory. Auto: High with enhanced "
                       "shadows, otherwise the game's own setting.",
        .type = Type::Enum,
        .defaultValue = 3,
        .options = {"auto", "game", "1024", "2048", "4096"},
        .optionLabels = {"Auto", "Game setting", "Low (1024)", "Medium (2048)", "High (4096)"},
        .page = "Lighting & Shadows", .section = "Sun shadows",
    });
    r.shadowsBias = &reg.add({
        .key = "shadows.bias",
        .label = "Shadow offset",
        .category = "Shadows",
        .description = "Moves shadows slightly away from surfaces. Raise it if lit surfaces show stripes; too high and "
                       "shadows come loose from their objects.",
        .type = Type::Float,
        .defaultValue = 0.5,
        .min = 0,
        .max = 4,
        .format = "%.2f",
        .flags = Flag::Advanced,
        .needs = {{"shadows.enabled", "true", false, true}},
        .page = "Lighting & Shadows", .section = "Sun shadows",
    });
    r.shadowsRange = &reg.add({
        .key = "shadows.range",
        .label = "Shadow distance",
        .category = "Shadows",
        .description = "How far away objects still cast sun shadows. Auto: 100 m with enhanced shadows, otherwise the game's "
                       "75 m. Farther costs more.",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 400,
        .format = "%.0f m",
        .autoLabel = "Auto",
        .flags = Flag::Advanced,
        .page = "Lighting & Shadows", .section = "Sun shadows",
    });
    r.shadowsRt = &reg.add({
        .key = "shadows.rt",
        .label = "Ray-traced shadows",
        .category = "Shadows",
        .description = "Shadows near the player traced against the world instead of read from the shadow maps: soft edges that "
                       "get sharper where an object touches the ground, leaves that shade their own shape, no shimmer. Near: "
                       "the closest 15 m. Extended: 35 m. Farther away the enhanced shadow maps take over. Needs a graphics "
                       "card with ray tracing.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "near", "extended"},
        .optionLabels = {"Off", "Near", "Extended"},
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .needs = {{"shadows.enabled", "true", false, true}},
        .page = "Ray Tracing", .section = "Traced sun shadows",
    });
    r.shadowsRtQuality = &reg.add({
        .key = "shadows.rt_quality",
        .label = "Ray-traced shadow quality",
        .category = "Shadows",
        .description = "Low: leaves cast solid shadows. Medium: leaves cast their own shape. High: twice the rays, smoother "
                       "soft edges.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 1,
        .needs = {{"shadows.rt", "off", true, true}},
        .page = "Ray Tracing", .section = "Traced sun shadows",
    });
    r.shadowsRtRange = &reg.add({
        .key = "shadows.rt_range",
        .label = "Ray-traced shadow distance",
        .category = "Shadows",
        .description = "How far from the player shadows are traced (the shadow maps take over beyond it). Auto: 15 m (near) or "
                       "35 m (extended). Farther costs more.",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 120,
        .format = "%.0f m",
        .autoLabel = "Auto",
        .flags = Flag::Hidden,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .needs = {{"shadows.rt", "off", true, true}},
        .page = "Ray Tracing", .section = "Traced sun shadows",
    });
    r.rtLodScale = &reg.add({
        .key = "rt.lod_scale",
        .label = "Ray tracing detail scale",
        .category = "Ray tracing",
        .description = "Scales the distances beyond which the traced world gets simpler: 0.3 starts close (small and fast, for "
                       "weaker graphics cards), 1 is the base distances, 0 = off (always full detail).",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 2,
        .format = "%.2f",
        .flags = Flag::Hidden,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .page = "Ray Tracing", .section = "Detail and memory",
    });
    r.rtMemoryBudget = &reg.add({
        .key = "rt.memory_budget",
        .label = "Ray tracing memory limit",
        .category = "Ray tracing",
        .description = "Limit for the graphics memory the traced world may use. Above it the simple copies start closer and far "
                       "objects give up their detailed version until it fits. 0 = no limit.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 4096,
        .format = "%.0f MB",
        .autoLabel = "No limit",
        .flags = Flag::Advanced,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .page = "Ray Tracing", .section = "Detail and memory",
    });
    r.rtPreset = &reg.add({
        .key = "rt.preset",
        .label = "Ray tracing quality",
        .category = "Ray tracing",
        .description = "One setting for the cost of everything that uses ray tracing: shadow, reflection and lighting quality and "
                       "ranges, how far the traced world keeps its detail and the memory it may use. Low is for weaker graphics "
                       "cards or when memory runs short, High traces the most. Changing any of those settings by hand makes it "
                       "Custom.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"low", "medium", "high", "custom"},
        .optionLabels = {"Low", "Medium", "High", "Custom"},
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 1,
        .page = "Ray Tracing", .section = "Quality", .order = -10,
    });
    r.shadowsDebug = &reg.add({
        .key = "shadows.debug",
        .label = "Shadow debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the enhanced shadows: the shadow mask shows the cascade instead of the shadow (dark "
                       "= the nearest cascade, lighter = farther ones, unshadowed beyond the shadow range), or only the cascades' or only "
                       "the terrain map's shadow.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "cascades", "cascade_term", "terrain_term", "rt_term", "rt_coverage"},
        .optionLabels = {"Off", "Cascades", "Cascade shadows only", "Terrain shadows only", "Ray-traced shadows only", "Ray-traced coverage"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });

    // ---- Graphics tab: global illumination (enh/gi.h, docs/research/rtgi.md) ----
    r.giEnabled = &reg.add({
        .key = "gi.enabled",
        .label = "Ray-traced lighting",
        .category = "Lighting",
        .description = "Light that bounces: sunlit walls and ground light their surroundings, rooms are darker away from "
                       "their windows and doors, caves and forests get their own shade, characters cast soft shade. Replaces "
                       "the game's flat ambient light. Needs a graphics card with ray tracing.",
        .type = Type::Bool,
        .defaultValue = 0,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .page = "Ray Tracing", .section = "Traced lighting",
    });
    r.giQuality = &reg.add({
        .key = "gi.quality",
        .label = "Lighting quality",
        .category = "Lighting",
        .description = "How quickly the bounced light follows changes (moving, time of day). Lower it if the frame rate drops.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .needs = {{"gi.enabled", "true", false, true}},
        .page = "Ray Tracing", .section = "Traced lighting",
    });
    r.giStrength = &reg.add({
        .key = "gi.strength",
        .label = "Bounced light strength",
        .category = "Lighting",
        .description = "0: the game's ambient light, 1: the traced light, between: a mix.",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 1,
        .format = "%.2f",
        .flags = Flag::Advanced,
        .needs = {{"gi.enabled", "true", false, true}},
        .page = "Ray Tracing", .section = "Traced lighting",
    });
    r.giLightmaps = &reg.add({
        .key = "gi.lightmaps",
        .label = "Baked indoor light",
        .category = "Lighting",
        .description = "Keep: interiors keep the game's own light of their lamps and fires (baked into the walls and added to "
                       "objects) on top of the traced light. Replace: only the traced light (much darker interiors).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"keep", "replace"},
        .optionLabels = {"Keep", "Replace"},
        .flags = Flag::Advanced,
        .needs = {{"gi.enabled", "true", false, true}},
        .page = "Ray Tracing", .section = "Traced lighting",
    });
    r.giScreen = &reg.add({
        .key = "gi.screen",
        .label = "Screen-space light detail",
        .category = "Lighting",
        .description = "Traces the bounced light from the surfaces on screen as well: exact shade where objects meet, light "
                       "that leaks less through walls. Lower it if the frame rate drops.",
        .type = Type::Enum,
        .defaultValue = 2,
        .options = {"off", "low", "medium", "high"},
        .optionLabels = {"Off", "Low", "Medium", "High"},
        .flags = Flag::Advanced,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .needs = {{"gi.enabled", "true", false, true}},
        .page = "Ray Tracing", .section = "Traced lighting",
    });
    r.giFoliage = &reg.add({
        .key = "gi.foliage",
        .label = "Leaves in the traced light",
        .category = "Lighting",
        .description = "Trees near you cast their real leaves into the traced light (shade under a canopy follows the leaves "
                       "and the wind, sunlit leaves tint the light they bounce). Off: branches and fronds only. Costs a little "
                       "CPU and GPU time per tree.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "near", "far"},
        .optionLabels = {"Off", "Near", "Far"},
        .flags = Flag::Advanced,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .needs = {{"gi.enabled", "true", false, true}},
        .page = "Ray Tracing", .section = "Traced lighting",
    });
    r.giReference = &reg.add({
        .key = "gi.reference",
        .label = "GI reference path tracer",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view: the ground truth of the ray-traced lighting. Paths with next-event estimation from the same "
                       "scene, materials and lights as the probes, accumulated while the camera and the light stand still (a "
                       "still image converges over a few hundred frames), dumped to bin/rvk/gi_ref when gi.reference_max samples "
                       "are reached. Views: the reference, the probe volume, the probes with the screen probes' factor, heat maps "
                       "(red = the probes are too bright, blue = too dark), the indirect light alone. Needs the screen probes.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "reference", "ddgi", "screen", "error", "reference_indirect", "screen_indirect", "error_ddgi"},
        .optionLabels = {"Off", "Reference", "Probe volume", "Probes + screen probes", "Error (probes + screen)", "Reference, indirect only",
                         "Probes + screen, indirect only", "Error (probe volume)"},
        .flags = Flag::NoAB | Flag::Debug,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .page = "Debug", .section = "GI reference",
    });
    r.giReferenceSpp = &reg.add({
        .key = "gi.reference_spp",
        .label = "Reference samples per frame",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Samples per pixel the reference path tracer adds each frame (costs GPU time: 1 sample at 1280x720 is about 1 ms).",
        .type = Type::Int,
        .defaultValue = 4,
        .min = 1,
        .max = 64,
        .flags = Flag::Debug | Flag::NoAB,
        .page = "Debug", .section = "GI reference",
    });
    r.giReferenceMax = &reg.add({
        .key = "gi.reference_max",
        .label = "Reference samples (total)",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The accumulation stops at this many samples per pixel and its planes are written to bin/rvk/gi_ref "
                       "(luminance-only dumps at 64, 128, ... on the way).",
        .type = Type::Int,
        .defaultValue = 1024,
        .min = 1,
        .max = 1000000,
        .flags = Flag::Debug | Flag::NoAB,
        .page = "Debug", .section = "GI reference",
    });
    r.giReferenceBounces = &reg.add({
        .key = "gi.reference_bounces",
        .label = "Reference bounces",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The most bounces of a reference path (Russian roulette ends most earlier).",
        .type = Type::Int,
        .defaultValue = 6,
        .min = 1,
        .max = 16,
        .flags = Flag::Debug | Flag::NoAB,
        .page = "Debug", .section = "GI reference",
    });
    r.giDebug = &reg.add({
        .key = "gi.debug",
        .label = "GI debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the ray-traced lighting: \"Irradiance\" shows the probes' light on a white surface "
                       "(geometric normal), \"Cascades\" the probe cascade each pixel uses (red, green, blue; dark red = no "
                       "probes: the game's ambient), \"Vertex AO\" the level's baked indoor factor (white = open air, black = the "
                       "game's indoor ambient).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "irradiance", "cascades", "vertex_ao"},
        .optionLabels = {"Off", "Irradiance", "Cascades", "Vertex AO"},
        .flags = Flag::NoAB | Flag::Debug,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .page = "Debug", .section = "Debug views",
    });

    r.aoEnabled = &reg.add({
        .key = "ao.enabled",
        .label = "Ambient occlusion",
        .category = "Lighting",
        .description = "Soft contact shade in corners, creases, under furniture and where objects meet the ground (on the "
                       "ambient light; works with and without ray-traced lighting).",
        .type = Type::Bool,
        .defaultValue = 0,
        .page = "Lighting & Shadows", .section = "Ambient occlusion",
    });
    r.aoMode = &reg.add({
        .key = "ao.mode",
        .label = "Ambient occlusion mode",
        .category = "Lighting",
        .description = "Replace: the game's own ambient occlusion is switched off and only the new one shades (on the ambient "
                       "light only: sunlit edges stay bright). Add: both shade (darker creases).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"replace", "add"},
        .optionLabels = {"Replace", "Add"},
        .flags = Flag::Advanced,
        .needs = {{"ao.enabled", "true", false, true}},
        .page = "Lighting & Shadows", .section = "Ambient occlusion",
    });
    r.aoStrength = &reg.add({
        .key = "ao.strength",
        .label = "Ambient occlusion strength",
        .category = "Lighting",
        .description = "How dark the contact shade gets.",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 1,
        .format = "%.2f",
        .flags = Flag::Advanced,
        .needs = {{"ao.enabled", "true", false, true}},
        .page = "Lighting & Shadows", .section = "Ambient occlusion",
    });
    // ---- Full-owned lighting (enh/light_table.h, docs/research/gpu-scene.md 9.7) ----
    r.lightingClustered = &reg.add({
        .key = "lighting.clustered",
        .label = "Clustered lights (Full)",
        .category = "Lighting",
        .tab = kTabDebug,
        .description = "Full mode only (takes effect at the next start; default on, Full mode only: Wrapper is unchanged): the world's lights go into a GPU light table with a "
                       "cluster grid and every lit surface takes its point lights per pixel from it, evaluated by the game's own shader, instead of "
                       "the few lights the game attaches to each object. Lights beyond the game's per-object reach light the surfaces they reach. "
                       "rvk.log `enh: lights:`.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Restart | Flag::Debug,
        .page = "Debug", .section = "Clustered lights",
    });
    r.lightingEnabled = &reg.add({
        .key = "lighting.enabled",
        .label = "Clustered lights on",
        .category = "Lighting",
        .tab = kTabDebug,
        .description = "Run-time switch of the light table while lighting.clustered was on at start (off = the game's own per-object lights: A/B).",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Clustered lights",
    });
    r.lightingFullLit = &reg.add({
        .key = "lighting.full_lit",
        .label = "Table lighting (Full)",
        .category = "Lighting",
        .tab = kTabDebug,
        .description = "Full mode with clustered lights (default on): every lit mesh / tree draw is composed with ONE pass of the 3-light "
                       "shader permutation and takes all its point lights from the light table per pixel: the game's per-object light lists are no longer "
                       "read, the additive passes of objects with more than 3 lights are gone and tree / mesh batches merge more. The picture differs "
                       "from the game's (more lights where the game gave an object none or few). Run-time switch. rvk.log `native: table lighting`.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Clustered lights",
    });
    r.lightingStyle = &reg.add({
        .key = "lighting.style",
        .label = "Light style",
        .category = "Lighting",
        .description = "Original: the game's light falloff, so the image reads like the game. Physical: lamps follow an inverse-square law with a smooth "
                       "reach (brighter close to a lamp, gentler far away) and surfaces get a physically based specular highlight with energy-conserving diffuse (the sun, sky and "
                       "exposure stay the game's). Needs Full mode with clustered lights; the shader code for it is loaded only when the game STARTS with Physical selected, "
                       "so a change takes effect at the next start.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"original", "physical"},
        .optionLabels = {"Original", "Physical"},
        .flags = Flag::Restart,
        .needs = {{"renderer.mode", "full", false, false}, {"lighting.clustered", "true", false, true}},
        .page = "Lighting & Shadows", .section = "Light style",
    });
    r.lightingDebug = &reg.add({
        .key = "lighting.debug",
        .label = "Lights debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the light table: \"Cluster counts\" shows the lights in each pixel's cluster (blue few, red 16+, black none), "
                       "\"Slots filled\" how many of the shader's light slots the table fills (red 1, green 2, yellow 3, dim blue none), \"Light shadows\" the ray-traced lamp shadows (white lit, red shadowed, dark blue no shadow-casting lamp).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "clusters", "slots", "shadows", "bake", "prebake", "path"},
        .optionLabels = {"Off", "Cluster counts", "Slots filled", "Light shadows", "Baked light shadow", "Colour before the baked-light shadow", "Cluster path used"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Clustered lights",
    });
    r.lightingShadows = &reg.add({
        .key = "lighting.light_shadows",
        .label = "Lamp shadows",
        .category = "Lighting",
        .description = "Ray-traced shadows of the lamps, torches and candles: the nearest lights (Nearby: 8 within 25 m, Many: 31 within 60 m) shade the surfaces they "
                       "reach with soft shadows traced against the world, so far more lights than the game's own three shadowed lights cast shadows. Full mode "
                       "with the clustered lights; needs a graphics card with ray tracing (without one the game's own point-light shadows stay).",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"off", "nearby", "many"},
        .optionLabels = {"Off", "Nearby", "Many"},
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
        .needs = {{"renderer.mode", "full", false, false}, {"lighting.clustered", "true", false, true}, {"lighting.full_lit", "true", false, true}},
        .page = "Ray Tracing", .section = "Lamp shadows",
    });
    r.lightingShadowQuality = &reg.add({
        .key = "lighting.light_shadow_quality",
        .label = "Lamp shadow quality",
        .category = "Lighting",
        .description = "Low: leaves and fences cast solid shadows. Medium: leaves cast their own shape. High: two rays per light, smoother soft edges.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .flags = Flag::Advanced,
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 1,
        .needs = {{"lighting.light_shadows", "off", true, true}},
        .page = "Ray Tracing", .section = "Lamp shadows",
    });
    r.gbufferDebug = &reg.add({
        .key = "gbuffer.debug",
        .label = "G-buffer debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the frame data: \"Normals\" the surfaces' normals (red tint = characters, black = no "
                       "material), \"Motion\" the motion vectors (hue = direction), \"AO\" the ambient occlusion.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "normals", "motion", "ao", "screen_gi", "material"},
        .optionLabels = {"Off", "Normals", "Motion", "AO", "Screen probes (GI factor)", "Material (pack: roughness / metalness)"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });

    // ---- Graphics tab: material pack (enh/material_pack.h, docs/research/material-pack.md) ----
    r.materialsPack = &reg.add({
        .key = "materials.pack",
        .label = "Material pack",
        .category = "Materials",
        .description = "Loads extra material data (roughness / metalness / emissive maps) from the folder bin\\rvk\\materialpack and gives "
                       "the materials in it a physically based specular response and sky reflections. Materials without an entry look "
                       "exactly as before. Takes effect at the next start.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Restart,
        .page = "Water & Materials", .section = "Material pack",
    });
    r.materialsQuality = &reg.add({
        .key = "materials.quality",
        .label = "Material reflections",
        .category = "Materials",
        .description = "Low: sun specular and sky reflection. Medium: + the nearby lights' specular. High: + reflections traced through the world "
                       "(smooth surfaces mirror the buildings, trees and ships around them, not only the sky; needs a graphics card with ray tracing).",
        .type = Type::Enum,
        .defaultValue = 2,
        .options = {"off", "low", "medium", "high"},
        .optionLabels = {"Off", "Low", "Medium", "High"},
        .optionRequirements = {0, 0, 0, caps::bit(caps::Cap::RayQuery)},
        .needs = {{"materials.pack", "true", false, true}},
        .page = "Water & Materials", .section = "Material pack",
    });
    r.materialsRtReflections = &reg.add({
        .key = "materials.rt_reflections",
        .label = "Traced material reflection detail",
        .category = "Materials",
        .description = "Detail of the traced reflections of the material pack's smooth surfaces (Material pack quality: High). Low traces one ray per four "
                       "pixels and shows foliage solid, High traces every pixel with alpha-tested leaves and shadows in the reflection.",
        .type = Type::Enum,
        .defaultValue = 2,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
        .needs = {{"materials.quality", "high", false, true}},
        .page = "Water & Materials", .section = "Traced reflections",
    });
    r.materialsRtRange = &reg.add({
        .key = "materials.rt_range",
        .label = "Traced reflection range",
        .category = "Materials",
        .description = "How far a reflection ray travels (metres); beyond it the sky is reflected.",
        .type = Type::Float,
        .defaultValue = 150,
        .min = 10,
        .max = 600,
        .format = "%.0f m",
        .flags = Flag::Advanced,
        .needs = {{"materials.quality", "high", false, true}},
        .page = "Water & Materials", .section = "Traced reflections",
    });
    r.materialsDerive = &reg.add({
        .key = "materials.derive_from_specular",
        .label = "Gloss from the game's specular data",
        .category = "Materials",
        .description = "Materials WITHOUT a pack entry that the game itself makes glossy (a specular map or colour with a specular power) get a "
                       "roughness derived from that data, so the sky and traced reflections and the G-buffer's material channel cover them too "
                       "(skin, cloth, wet ground layers). The game's own highlights stay as they are. Needs the material pack. Materials with a "
                       "pack entry, and those without specular data, are not changed.",
        .type = Type::Bool,
        .defaultValue = 1,
        .page = "Water & Materials", .section = "Material pack",
    });
    r.materialsBudget = &reg.add({
        .key = "materials.budget",
        .label = "Pack texture memory",
        .category = "Materials",
        .description = "Maximum memory of the decoded pack textures. Takes effect at the next start.",
        .type = Type::Int,
        .defaultValue = 256,
        .min = 16,
        .max = 2048,
        .format = "%.0f MiB",
        .flags = Flag::Restart | Flag::Advanced,
        .page = "Water & Materials", .section = "Material pack",
    });
    r.materialsGlass = &reg.add({
        .key = "materials.glass",
        .label = "Traced glass",
        .category = "Materials",
        .description = "Windows and bottles of the material pack's glass entries reflect the world through ray tracing (Fresnel: more at a glancing "
                       "angle) and bottles refract what is behind them. Needs Material pack quality High. Off: the game's plain blended glass.",
        .type = Type::Bool,
        .defaultValue = 1,
        .needs = {{"materials.quality", "high", false, true}},
        .page = "Water & Materials", .section = "Traced reflections",
    });
    r.materialsRainWetness = &reg.add({
        .key = "materials.rain_wetness",
        .label = "Rain",
        .category = "Materials",
        .description = "How much rain wets the world while the traced reflections run (Material pack quality: High): surfaces in the open get smoother, "
                       "darker and shinier, and dry again slowly after the rain. 0 = the rain leaves no trace.",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 1,
        .format = "%.2f",
        .needs = {{"materials.quality", "high", false, true}},
        .page = "Water & Materials", .section = "Rain",
    });
    r.materialsRainForce = &reg.add({
        .key = "materials.rain_force",
        .label = "Force rain",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Test switch: treats the weather as this much rain (0 = the game's weather, 1 = pouring) so the wet look and the puddles can be "
                       "checked in sunshine.",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 1,
        .format = "%.2f",
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.materialsAlternate = &reg.add({
        .key = "materials.alternate",
        .label = "Pack variants on / off every N frames",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Test switch: every N frames the material pack's pixel shader variants are bypassed (the engine's own shaders draw), so two "
                       "pictures N frames apart in a frozen scene show exactly what the variants change where no entry matches. 0 = off.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 600,
        .format = "%.0f frames",
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.materialsDebug = &reg.add({
        .key = "materials.debug",
        .label = "Material pack debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the pack data on the draws that have an entry: roughness, metalness, the packed map (red occlusion, "
                       "green roughness, blue metalness) or a green tint (coverage: which draws matched).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "roughness", "metalness", "orm", "coverage"},
        .optionLabels = {"Off", "Roughness", "Metalness", "Packed map", "Coverage"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });

    // ---- Graphics tab: upscaling / anti-aliasing (enh/upscale.h) ----
    r.upscaleMode = &reg.add({
        .key = "upscale.mode",
        .label = "Anti-aliasing / upscaling",
        .category = "Image",
        .description = "AMD FidelityFX Super Resolution: a temporal filter that removes jagged edges and shimmer. Native AA "
                       "renders at the screen's resolution; High quality to Ultra performance render the 3D scene at 1/1.2 .. 1/3 of "
                       "it per dimension (faster) and upscale it; the menus and the HUD stay sharp. \"Jitter only\" shakes "
                       "the camera without the filter (developer test).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "native_aa", "high_quality", "quality", "balanced", "performance", "ultra_performance", "jitter_only"},
        .optionLabels = {"Off", "Native AA (FSR)", "High quality (1.2x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2x)",
                         "Ultra performance (3x)", "Jitter only (test)"},
        .page = "Image", .section = "Upscaling and anti-aliasing",
    });

    r.upscaleProvider = &reg.add({
        .key = "upscale.provider",
        .label = "Upscaler",
        .category = "Image",
        .description = "Which AMD FidelityFX upscaler runs: \"FSR 3.1\" (the analytic filter, the default) or \"FSR 4\" (a neural "
                       "network: sharper and steadier in motion, costs more GPU time; needs a Radeon RX 6000 series or newer, "
                       "and the GPU driver's AMD DLL support; experimental). Falls back to FSR 3.1 when the GPU is not supported.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"fsr3", "fsr4"},
        .optionLabels = {"FSR 3.1", "FSR 4 (neural, experimental)"},
        .flags = Flag::Advanced,
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Image", .section = "Upscaling and anti-aliasing",
    });
    r.upscaleObjectMotion = &reg.add({
        .key = "upscale.object_motion",
        .label = "Motion of characters and objects",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Tells FSR how characters (and with \"Characters and objects\" also moving rigid objects such as a drawn "
                       "weapon) really move, so they stay sharp instead of blurring while they animate.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"off", "characters", "all"},
        .optionLabels = {"Off", "Characters", "Characters and objects"},
        .flags = Flag::Debug,
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Debug", .section = "Debug views",
    });
    r.upscaleReactive = &reg.add({
        .key = "upscale.reactive",
        .label = "Clean up effects",
        .category = "Image",
        .description = "Keeps particles, rain, spells and spray from leaving smeared trails behind them: FSR trusts its history "
                       "less where such effects are drawn.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Advanced,
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Image", .section = "Upscaling and anti-aliasing",
    });
    r.upscaleSharpness = &reg.add({
        .key = "upscale.sharpness",
        .label = "Sharpening",
        .category = "Image",
        .description = "How much FSR's sharpening pass (RCAS) crisps the picture after the temporal filter. 0 = off, 0.4 = about "
                       "the detail of the game's own picture, 1 = crunchy.",
        .type = Type::Float,
        .defaultValue = 0.4,
        .min = 0,
        .max = 1,
        .format = "%.2f",
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Image", .section = "Upscaling and anti-aliasing",
    });
    r.upscaleExposure = &reg.add({
        .key = "upscale.exposure",
        .label = "Exposure",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Which brightness FSR weighs bright and dark pixels with: \"Game\" gives it the exposure the game's own tone "
                       "mapping uses (steady, and what you actually see); \"Automatic\" lets FSR estimate it from every frame.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"engine", "auto"},
        .optionLabels = {"Game", "Automatic (FSR)"},
        .flags = Flag::Debug,
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Debug", .section = "Debug views",
    });
    r.upscaleJitterFlip = &reg.add({
        .key = "upscale.jitter_flip",
        .label = "Jitter sign (test)",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer test: flips the sign of the camera jitter on the geometry (not in what FSR is told).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"none", "x", "y", "xy"},
        .optionLabels = {"None", "X", "Y", "X and Y"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Debug views",
    });
    r.upscaleCutoutThicken = &reg.add({
        .key = "upscale.cutout_thicken",
        .label = "Steadier thin cut-outs",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Below native resolution, cut-out textures with strands thinner than a pixel (fishing nets, fine leaves) "
                       "flicker under FSR. This keeps such strands about a pixel wide: steadier, but they look heavier and less "
                       "see-through. Scaled with the render scale (nothing at native AA). 0 = off; 0.15 to 0.25 is a mild setting.",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 0.5,
        .format = "%.2f",
        .flags = Flag::Debug,
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Debug", .section = "Debug views",
    });
    r.upscaleAlphaDither = &reg.add({
        .key = "upscale.alpha_dither",
        .label = "Hashed alpha test",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Experiment for thin cut-out textures (fishing nets) under FSR: the cut-out of the jittered scene draws (the "
                       "z pre-pass texkill, the alpha test) passes with the texture's coverage of the pixel against noise that "
                       "changes every frame, instead of a fixed threshold. About 20 % less flicker on nets, but softer.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug,
        .needs = {{"upscale.mode", "off", true, true}},
        .page = "Debug", .section = "Debug views",
    });

    // ---- Graphics tab: frame generation (enh/framegen.h; docs/research/frame-generation.md). AMD's frame generation swapchain presents
    // the frames and paces them; the manual path (framegen.manual) is the developer view of the same generator.
    r.framegenMode = &reg.add({
        .key = "framegen.mode",
        .label = "Frame generation",
        .category = "Image",
        .description = "AMD FidelityFX frame generation: an extra frame is computed between every two rendered frames, so the picture "
                       "is shown at twice the rate the game renders at (60 rendered = 120 shown). The game itself still runs at the "
                       "lower rate and your input shows about one rendered frame later; best from about 60 rendered frames per second. "
                       "FSR 3 works on any recent graphics card (it needs the separate frame generation download in the rvk "
                       "folder); FSR 4 (neural) only on a Radeon RX 9000 series, it is greyed out otherwise. Needs the borderless "
                       "window (exclusive fullscreen is switched off while it is on). The frame rate limit in Display then limits the "
                       "frames shown, the game renders half as many. Not used in menus and loading screens. A driver frame limiter "
                       "for the game (AMD Radeon Chill) counts the generated frames too: switch it off for Risen.exe.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "fsr3", "fsr4"},
        .optionLabels = {"Off", "FSR 3", "FSR 4 (neural)"},
        .requirements = caps::bit(caps::Cap::FrameGeneration),
        .unmetValue = 0,
        .optionRequirements = {0, 0, caps::bit(caps::Cap::FrameGenerationMl)},
        .page = "Image", .section = "Frame generation",
    });
    r.framegenManual = &reg.add({
        .key = "framegen.manual",
        .label = "Present generated frames ourselves (test)",
        .category = "Frame generation",
        .tab = kTabDebug,
        .description = "Developer test (also the fallback when AMD's frame generation swapchain cannot be created): the generated frame "
                       "is computed after the upscaler, shown first and the real frame half a frame later, both through our own "
                       "swapchain (simple timed spacing, not AMD's pacing). Generated frames can be captured (screenshots "
                       "frame_<n>_gen.png).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug,
        .requirements = caps::bit(caps::Cap::FrameGeneration),
        .unmetValue = 0,
        .needs = {{"framegen.mode", "off", true, true}},
        .page = "Debug", .section = "Debug views",
    });
    r.framegenDebug = &reg.add({
        .key = "framegen.debug",
        .label = "Frame generation view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer: shows an input of the frame generation instead of the frame: the motion vectors (hue = direction, "
                       "brightness = length), the depth, the copy of the frame without the HUD, or the generated frame itself.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "motion", "depth", "hudless", "generated"},
        .optionLabels = {"Off", "Motion vectors", "Depth", "HUD-less copy", "Generated frame"},
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"framegen.mode", "off", true, true}},
        .page = "Debug", .section = "Debug views",
    });

    // ---- Graphics tab: detail distances (engine/lod.h) ----
    r.lodTrees = &reg.add({
        .key = "lod.trees",
        .label = "Tree detail distance",
        .category = "Detail",
        .description = "How far away trees and palms keep their full detail before they thin out and turn into flat distant "
                       "images. 1 = the game's distances (10-45 m full detail, flat beyond 60-135 m, per tree). Farther costs "
                       "some frame rate in forests.",
        .type = Type::Float,
        .defaultValue = 2,
        .min = 1,
        .max = 4,
        .format = "%.1fx",
        .page = "Display", .section = "Draw distances",
    });
    r.lodMeshes = &reg.add({
        .key = "lod.meshes",
        .label = "Object detail distance",
        .category = "Detail",
        .description = "How far away buildings, rocks and other objects keep their full detail before simpler versions "
                       "replace them. 1 = the game's distance.",
        .type = Type::Float,
        .defaultValue = 2,
        .min = 1,
        .max = 4,
        .format = "%.1fx",
        .page = "Display", .section = "Draw distances",
    });
    r.shadowStride = &reg.add({
        .key = "debug.shadow_stride",
        .label = "Sun shadow maps redrawn every n-th frame",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The game redraws all sun cascades every frame. While the camera stands still (or barely moves) the maps "
                       "are the same: every n-th frame (and at once when the camera moved or turned) is enough. 1 = every frame.",
        .type = Type::Int,
        .defaultValue = 2,
        .min = 1,
        .max = 4,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.rtShadowSync = &reg.add({
        .key = "debug.rt_shadow_sync",
        .label = "Sun shadow maps every frame with ray-traced shadows",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "While the ray-traced shadows are on, the sun cascades are redrawn every frame (the stride setting is "
                       "ignored): the characters are drawn into them before the main view, which gives the ray-traced shadows "
                       "the characters' pose of this frame instead of the previous one. Off: the stride decides.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.rtSkin = &reg.add({
        .key = "debug.rt_skin",
        .label = "Characters skinned by compute for ray tracing",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The ray tracing scene (shadows, GI, reflections) skins the characters itself from the draws' vertex buffers "
                       "and bone matrices, once per frame before the scene is built: this frame's pose, no sun cascade redraw. "
                       "Off: the vertex shaders' captures (this frame's from the cascades when they are drawn, else the previous frame's).",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.rtSkinVerify = &reg.add({
        .key = "debug.rt_skin_verify",
        .label = "Verify the compute skinning",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Compares the compute skinning with the vertex shader's own result on the sun cascades' draws and logs the "
                       "largest difference (rvk.log `rt: skin verify`). Costs a second capture per character.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.nativeFrameTap = &reg.add({
        .key = "native.frame_tap",
        .label = "Native frame: op tap",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Reads the main view's render ops as the engine executes them (the sorted lists of eCFrustumDatabase::Execute) "
                       "and maps every draw of the view to its op; rvk.log `native: frame tap:` every 5 s, the op lists with -LogFrame. "
                       "Observes only.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedObserve = &reg.add({
        .key = "native.owned_observe",
        .label = "Frame ownership: observe requests",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Records every RequestRenderOps of the mesh / skinned / SpeedTree / tree billboard / vegetation contexts per view (the visible "
                       "lists of the owned frame, F0) and verifies them against the databases' op lists and the frame tap; rvk.log `native: owned "
                       "observe (5 s)`. Observes only. The hooks are installed at start only when this is on then (turning it on later needs a "
                       "restart); Full.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeStaticOps = &reg.add({
        .key = "native.static_ops",
        .label = "Native frame: static ops",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The main view's z pre-pass and solid draws of static meshes are drawn by the renderer itself from engine data "
                       "(the engine's own shaders, constants and states composed natively); the engine's draws of those ops are dropped. "
                       "rvk.log `native: static ops:` every 5 s. renderer.mode = full turns it on; this override turns it on in "
                       "Wrapper too (bisecting).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeStaticOpsCompare = &reg.add({
        .key = "native.static_ops_compare",
        .label = "Native frame: static ops compare",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Composes the static mesh ops' draws and compares each with the draw the engine makes (shaders, constants, "
                       "states, samplers, buffers, ranges); rvk.log `native: static ops: compare` every 5 s with the first mismatches. "
                       "With native.static_ops on, the dropped engine draws are compared with what the renderer drew instead.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeSceneOps = &reg.add({
        .key = "native.scene_ops",
        .label = "Native frame: scene ops",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The rest of the main view's mesh-like work (alpha and refracted mesh ops, entity overlay passes, decals, the sky, skinned actors) is drawn by the "
                       "renderer itself from engine data; the engine's draws of those ops are dropped. Groups: native.scene_ops_groups. "
                       "rvk.log `native: scene ops` every 5 s. renderer.mode = full turns it on; this override turns it on in Wrapper "
                       "too (bisecting).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeSceneOpsCompare = &reg.add({
        .key = "native.scene_ops_compare",
        .label = "Native frame: scene ops compare",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Composes the scene ops' draws and compares each with the draw the engine makes; with native.scene_ops on, the "
                       "dropped engine draws are compared with what the renderer drew instead.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeSceneOpsGroups = &reg.add({
        .key = "native.scene_ops_groups",
        .label = "Native frame: scene op groups",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Bit mask of the scene ops' groups (in Full mode and with native.scene_ops): 1 alpha / refracted mesh ops, 2 entity overlay passes (also of the static ops "
                       "while scene ops are on), 4 decals (eCDecalAdmin::RenderDecals), 8 the sky dome op (stars, sun, moon, dome), 16 the opaque "
                       "billboards (eCBillboardAdmin::RenderBillboards), 32 the sky environment cube's faces, 64 the skinned actors' ops "
                       "(eCRenderContextAnimation2: bones, morphs, body parts, overlays), 128 the SpeedTree ops (branches, fronds, leaves) and "
                       "the tree billboards' ops, 256 the vegetation ops (grass batches), 512 the shadow maps' z ops (sun cascades, terrain "
                       "variance map, point-light atlas cells), 1024 the effect draws around the shadows (the post_SSM shadow mask chain: "
                       "SSAO, cascades, terrain term, blur; its depth copy; the terrain variance map's Blur7H / V), 2048 the shadow views' setup "
                       "(the cascade fit and shadow cameras compared; the maps' target binds, viewports, clears and resets), 4096 the particle ops (CPU "
                       "quads), 8192 the strips (trails) and light streaks, 16384 the precipitation (rain / snow, the late list), 32768 the post chain "
                       "(the image-effect graphs' filters: luminance, bloom, streaks, tone map, DoF, overlay effects), 65536 the sprite batcher "
                       "(GUI, text, cursor, loading screens, videos).",
        .type = Type::Int,
        .defaultValue = 131071,
        .min = 0,
        .max = 131071,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeIllumTable = &reg.add({
        .key = "native.illum_table",
        .label = "Native frame: illumination table",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only (takes effect at the next start; frame-ownership.md F0, no picture change): a GPU table of the world's illumination "
                       "objects and light sets (raw static lights / SH / f / set; the set's sun / SH / indoor / dynamic lights), kept current from the engine's "
                       "UpdateIllumination / SetLightSet / UpdateLightSets, and a check that composes every lit draw's lighting registers c14..c37 from the "
                       "records and compares them with what the native composition wrote. rvk.log `native: illum table (5 s)`, DIFFERING must stay 0.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Restart | Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeMeshParts = &reg.add({
        .key = "native.mesh_parts",
        .label = "Native frame: mesh part + layout tables",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only (takes effect at the next start; frame-ownership.md F0, no picture change): GPU tables of the static meshes' parts "
                       "(sub-mesh index range, index / vertex stream addresses in the mesh arena, base vertex, layout, material key) and of the vertex "
                       "layouts (per-semantic pull offsets / formats), and a check that compares every composed static draw with its record and, every "
                       "240th frame, fetches indices / positions / normals / uvs of random vertices on the GPU through the tables and compares them with "
                       "the CPU. rvk.log `native: mesh parts (5 s)`, DIFFERING must stay 0.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Restart | Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeLightingImages = &reg.add({
        .key = "native.lighting_images",
        .label = "Native frame: lighting register images",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only (takes effect at the next start; frame-ownership.md 7.9 / F1b, no picture change; needs native.illum_table): one image of the "
                       "lighting registers c14..c37 per distinct (illumination, pass parameters) the native composition used, composed on the GPU every frame "
                       "from the illumination table's records by one compute pass (before the frame's draws), and a check (every 60th frame) that reads the "
                       "images back and compares them with the CPU composition and with the registers the draws got. rvk.log `native: lighting images (5 s)`, "
                       "both DIFFERING counts must stay 0.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Restart | Flag::NoAB | Flag::Debug,
        .needs = {{"native.illum_table", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeMaterialTable = &reg.add({
        .key = "native.material_table",
        .label = "Native frame: material table",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only (takes effect at the next start; frame-ownership.md F0, no picture change): a GPU table with one record per engine "
                       "material as the native composition draws it (VS / PS pool registers, sampler slots -> bindless texture + sampler states, mask "
                       "reference, blend mode, graph ids), re-read once per frame at the material's first draw, and a check that composes every static draw's "
                       "material registers / texture bindings from the record and compares them with what the composition set. rvk.log `native: material "
                       "table (5 s)`, DIFFERING must stay 0.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::Restart | Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedWindows = &reg.add({
        .key = "native.owned_windows",
        .label = "Native frame: owned register windows",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs the material table (frame-ownership.md F1a, no picture change): the native static draws read their material "
                       "registers (PS c6..c13, VS c232..) from the material table's records and the camera / fog registers (c0..c11 / c0..c4) from a per-view "
                       "block instead of uploading them per draw (the other windows stay uploaded). debug.split_verify compares every served window with the "
                       "register file. rvk.log `native: owned windows (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.material_table", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedObjects = &reg.add({
        .key = "native.owned_objects",
        .label = "Native frame: owned object registers",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs owned windows (frame-ownership.md F2c-1, no picture change): the native static ops' non-instanced mesh draws read the "
                       "object registers (VS c31..c35: fade, world matrix) from a persistent object table by scene-table slot and the admin registers (VS c12..c30) "
                       "from the per-view block instead of uploading them per draw. debug.split_verify compares every served window with the register file. "
                       "rvk.log `native: owned objects (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_windows", "true", false, true}, {"native.material_table", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedRecords = &reg.add({
        .key = "native.owned_records",
        .label = "Native frame: owned op records",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs owned objects, lighting images and table lighting (frame-ownership.md F2c-2, no picture change): the static ops' main-view z / "
                       "solid mesh ops are snapshotted into op records; on later frames the draws are derived again from the record + the per-frame inputs and compared "
                       "with the composed ones (nothing derived is submitted), and the read log of each record is audited against the engine's memory. "
                       "rvk.log `native: owned records (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_objects", "true", false, true}, {"native.lighting_images", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedStatic = &reg.add({
        .key = "native.owned_static",
        .label = "Native frame: owned static draws",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs the owned op records, scene draws and table lighting (frame-ownership.md F2c-3): the main view's static z / solid mesh ops whose "
                       "record is valid leave the engine's op lists at the view's start and are drawn from the records without a composition (DrawBlock derived, every "
                       "register window from the tables), when the z / solid pass begins; their side effects (resource touches, lightmap binds, stream 3) run for each. "
                       "native.owned_check frames run the old path and compare; a difference turns it off. rvk.log `native: owned static (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_records", "true", false, true}, {"native.scene_draws", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedPersistent = &reg.add({
        .key = "native.owned_persistent",
        .label = "Native frame: persistent owned draws",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs owned static draws and scene ABI v2 (frame-ownership.md F2c-4a): the owned draws' DrawRecords and DrawBlocks stay in persistent GPU tables and are "
                       "rewritten only when an input of their derivation changed (the device's pass state, the draw's textures / buffers / windows); a frame writes one reference list + "
                       "argument block and queues ONE indirect draw per bucket. native.owned_check frames compare the persistent content with a fresh derivation (MISS must stay 0; a MISS "
                       "turns owned_static off) and a rolling scrub re-derives 16 draws per frame. rvk.log `native: owned persistent (5 s)`, `native: draw record table (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_static", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeClusters = &reg.add({
        .key = "native.clusters",
        .label = "Native frame: mesh clusters",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "F5-1 (frame-ownership.md 7.33; the cull of F5-2 will read it): static rigid opaque meshes of >= 1,024 triangles per draw (z pre-pass, solid, shadow maps) get ~128-triangle "
                       "clusters built on a worker thread (meshoptimizer, cached in bin/rvk/cache/clusters), a copy of their indices in cluster order in the mesh arena (the engine's own copy stays; "
                       "the same triangles with the same vertex order, so the solid pass's depth EQUAL holds) which the draw uses, and one bounding sphere per cluster in a GPU table. "
                       "Nothing exists while off. rvk.log `native: clusters (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug | Flag::Restart,  // (7.47: retained draws / records keep the index buffers they were made with: a run-time toggle cannot take effect)
        .page = "Debug", .section = "Native frame",
    });
    r.nativeClusterCull = &reg.add({
        .key = "native.cluster_cull",
        .label = "Native frame: cluster cull",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "F5-2 (frame-ownership.md 7.34), Full mode, needs native.clusters + persistent owned draws: the owned z / solid draws of clustered meshes are culled per cluster on the GPU "
                       "(the compaction pass tests each cluster's bounding sphere against the main view's frustum, widened by a margin for the camera jitter) and the visible clusters are drawn "
                       "as at most 8 runs per draw (indirect arguments); the z pre-pass and the solid pass cull with the same inputs, so the solid pass's depth EQUAL holds. Check frames "
                       "read the GPU's result back and compare it with the exact CPU test (false negatives must be 0). rvk.log `native: owned compaction (5 s)` + `cluster cull`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug | Flag::Restart,  // (7.47: retained draws / records keep the index buffers they were made with: a run-time toggle cannot take effect)
        .needs = {{"native.clusters", "true", false, true}, {"native.owned_persistent", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeClusterCullShadows = &reg.add({
        .key = "native.cluster_cull_shadows",
        .label = "Native frame: cluster cull in the sun cascades",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "F3-2 (frame-ownership.md 7.37), Full mode, needs native.cluster_cull + native.owned_shadows: the owned z draws of clustered meshes in the sun cascades are culled per cluster "
                       "on the GPU against the cascade's four SIDE planes only (near and far stay open: a caster between the light and the cascade's near plane must stay), the visible clusters "
                       "are drawn as at most 8 runs per draw. Check frames read the GPU's result back and compare it with the exact CPU test (false negatives must be 0). Off = the cascades draw "
                       "whole ranges. rvk.log `native: cluster cull (5 s)` (the [cascades] triangle counts).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug | Flag::Restart,  // (7.47: retained draws / records keep the index buffers they were made with: a run-time toggle cannot take effect)
        .needs = {{"native.cluster_cull", "true", false, true}, {"native.owned_shadows", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedCheck = &reg.add({
        .key = "native.owned_check",
        .label = "Native frame: owned static check interval",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "With native.owned_static: every N-th frame (and the frame before it) no op leaves the lists: the old path composes and draws them and the record-derived "
                       "draws are compared with the composed ones (rvk.log `native: owned records`, DIFFERING must stay 0; a difference turns owned_static off); 0 = never.",
        .type = Type::Int,
        .defaultValue = 10,
        .min = 0,
        .max = 1000,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativePictureCheck = &reg.add({
        .key = "native.picture_check",
        .label = "Native frame: owned picture check",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Debug, Full (frame-ownership.md 7.47): at AfterOpaque of the main view a compute pass stores the opaque scene (colour + linear depth) of every composed frame (the "
                       "native.owned_check frames) as a reference and compares each owned frame's with it (same camera, still scene: time frozen, no upscaling jitter) and each composed frame with the previous "
                       "composed one (the noise floor). Two scene-size images + a few words exist only while it is on. rvk.log `native: owned picture check (5 s)`: differing pixels, the largest "
                       "differences, the bounding box, a verdict (FAILED = owned differs beyond the control).",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedTrees = &reg.add({
        .key = "native.owned_trees",
        .label = "Native frame: owned SpeedTrees",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs owned_persistent (frame-ownership.md 7.25, F4-2 / F4-3): the main view's SpeedTree z / solid ops are retained as one draw template per LOD bucket "
                       "(composed frames compare the plan and the templates with the composition: DIFFERING must stay 0, a difference turns it off); on the other frames the ops leave the lists at the "
                       "view's beginning and the pass draws every instance from the engine's tree objects with one row + record per instance and a few indirect calls (needs table lighting; "
                       "trees stay composed while the RT scene takes tree draws). Off = nothing changes. rvk.log `native: owned trees (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_persistent", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedShadows = &reg.add({
        .key = "native.owned_shadows",
        .label = "Native frame: owned shadow views",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs persistent owned draws (frame-ownership.md 7.36, F3-1): the sun cascades' static mesh z ops get op records per cascade (like the main view's) and, "
                       "on the frames where the stamps hold, leave the engine's z list at the view's start and are drawn from persistent DrawRecords (cascade view slots 4..6 of the view "
                       "table) by one indirect draw per bucket, without a composition. native.owned_check frames compare with the composition (DIFFERING / MISS must stay 0). Respects "
                       "debug.shadow_stride (only cascades the engine renders are drawn). Off = nothing changes. rvk.log `native: owned shadows (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_persistent", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedVegetation = &reg.add({
        .key = "native.owned_vegetation",
        .label = "Native frame: owned vegetation",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs the packet cache (frame-ownership.md 7.40, F7-1): the main view's vegetation (grass) ops keep their converted instance stream in a "
                       "persistent GPU buffer per op, rebuilt only when the engine's source instance bytes changed (an exact compare every frame; every 10th frame also "
                       "re-converts and compares); the grass draws then carry no CPU bytes and are replayed from the packet cache. Off = nothing changes. "
                       "rvk.log `native: owned vegetation (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedVegetationOps = &reg.add({
        .key = "native.owned_vegetation_ops",
        .label = "Native frame: owned vegetation draws",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs native.owned_vegetation and persistent owned draws (frame-ownership.md 7.42, F7-2): the main view's vegetation (grass) ops are retained as draw "
                       "templates per (type, pass, sun) bucket; composed frames compare the plan and the templates with the composition (DIFFERING must stay 0, a difference turns it off); on the other "
                       "frames the ops leave the lists at the view's beginning and the pass draws every batch from the engine's grid nodes: one DrawRecord per batch (lighting entry), the retained "
                       "converted instance bytes of all ops in one ring block (stream 1), a few indirect calls. Needs table lighting. Off = nothing changes. rvk.log `native: owned vegetation draws (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_persistent", "true", false, true}, {"native.owned_vegetation", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedActors = &reg.add({
        .key = "native.owned_actors",
        .label = "Native frame: owned skinned actors",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs persistent owned draws and owned objects (frame-ownership.md 7.41, F6-1): the skinned actors' z / solid ops of the main view and z ops of the sun "
                       "cascades get op records (like the static meshes') and, on the frames where the stamps hold, leave the engine's lists at the view's start and are drawn from "
                       "persistent DrawRecords by one indirect draw per bucket, without a composition. The per-frame part is one persistent register image per actor part in the bone "
                       "table (c31 morph weights, c32.w fade, the primitive's bone rows from the engine's palette) that every view of the frame reads as VS window 2. Ops with an "
                       "overlay flag (the highlighted focus NPC), a fade < 1 or a changed morph target stay composed. native.owned_check frames compare with the composition "
                       "(DIFFERING / MISS must stay 0). Off = nothing changes. rvk.log `native: owned actors (5 s)`, `native: bone table (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_persistent", "true", false, true}, {"native.owned_objects", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedInstances = &reg.add({
        .key = "native.owned_instances",
        .label = "Native frame: owned instance lists",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs persistent owned draws (frame-ownership.md 14.19, F2f): the main view's instanced mesh ops (render-op node 0xffffff: one draw per render "
                       "instance, the engine's hardware batches are composed per instance too while this is on) get op records whose stamp holds every instance's record (matrix, "
                       "switch, illumination, flags) and are drawn from persistent DrawRecords like the other static meshes; each draw's VS window 2 (c32.w, c33..c35) is an immutable "
                       "image in the instance row table. The blended terrain layers of the solid list are owned too, drawn from their records at the engine's place in the list "
                       "(order kept). native.owned_check frames compare with the composition (DIFFERING / MISS must stay 0). Off = nothing changes. rvk.log `native: owned instances (5 s)`, "
                       "`native: instance rows (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_persistent", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnedBillboards = &reg.add({
        .key = "native.owned_billboards",
        .label = "Native frame: owned tree billboards",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only, needs owned windows, lighting images and scene draws (frame-ownership.md F4-1): the main view's tree billboard ops (z / solid) are "
                       "retained as op records (draws, constants image c32..c36 + c37+j, lighting, side effects); on frames where the stamps hold the draws are submitted "
                       "from the record without composing them, every native.owned_check-th frame composes and compares (DIFFERING must stay 0). Off = nothing changes. "
                       "rvk.log `native: owned billboards (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_windows", "true", false, true}, {"native.lighting_images", "true", false, true}, {"native.scene_draws", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeSceneDraws = &reg.add({
        .key = "native.scene_draws",
        .label = "Native frame: scene draws",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode only (frame-ownership.md F2b-1, no picture change): the native static ops' main-view z / solid draws are recorded as DrawRecords "
                       "(register window bases, DrawBlock, flags) and drawn by one indirect call per run of consecutive draws with the same pipeline, vertex / index "
                       "buffers and state, through the scene form of their shaders (needs vertex input location 24 and indirect first instance). "
                       "rvk.log `native: scene draws (5 s)`.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeSceneDrawsCheck = &reg.add({
        .key = "native.scene_draws_check",
        .label = "Native frame: scene draws check interval",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "With native.scene_draws: every N-th frame the GPU copies the record and argument blocks of that frame into a read-back buffer and they are "
                       "compared with what the CPU wrote (rvk.log `native: scene draws check`, DIFFERING must stay 0); 0 = never.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 1000,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeLean = &reg.add({
        .key = "native.lean",
        .label = "Native frame: lean ops",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "O2a / O2b (docs/research/engine-frame/opt-plan.md; default on, acts where a group takes over: Full): for the "
                       "composed ops (static / alpha / refracted meshes, overlays, decals, sky, billboards, sky cube faces, skinned actors, "
                       "SpeedTrees, vegetation, the shadow maps' z ops, particles, strips, light streaks, rain) the engine's render code is "
                       "not run at all; its side effects (texture / material touches, lightmaps, stream 3, the instance sort, the "
                       "billboard queue reset, the sky's clouds, the actors' palettes, the tree queues' sort and MaskReference, the "
                       "vegetation cursor, the particles' master / in-slot simulation / inactivity timer / depth sort / first-use "
                       "creation) are run by the renderer. Off with a compare unless native.lean_check is set. rvk.log `native: lean` "
                       "every 5 s.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeLeanCheck = &reg.add({
        .key = "native.lean_check",
        .label = "Native frame: lean check interval",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "With native.lean: every N-th frame the engine's code runs as before (its draws dropped), so that a compare setting "
                       "proves the composition still equals the engine's on those frames; 0 = never.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 1000,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativePacketCache = &reg.add({
        .key = "native.packet_cache",
        .label = "Native frame: retained draw packets",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Full mode: the finished packet of a composed static-mesh / tree draw of the main view is kept and replayed while its description is unchanged "
                       "(prepareDraw and most state setters skipped). Off, on (default), on + every 10th frame the fresh packet is compared with the retained one, "
                       "verify every frame (rvk.log `native: packet cache`, DIFFERING must stay 0), no variants = on without retaining the variant draws (GI / material pack / billboard fade: the "
                       "behaviour before retained-signals.md 13, A/B). Off while a compare, the frame tap or coverage is on, and per draw "
                       "while an enhancement needs prepareDraw (replacers, G-buffer variants, tint, upscaling jitter).",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"off", "on", "verify_10", "verify_all", "no_variants"},
        .optionLabels = {"Off", "On", "On + verify every 10th frame", "On + verify every frame", "On without the variant draws (A/B)"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeDepthZ = &reg.add({
        .key = "native.depth_z",
        .label = "Native frame: depth-only z passes",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "S-6 (docs/research/frame-ownership.md 14.10; Full): colour-less z passes (the sun cascades) draw opaque casters without a pixel shader (theirs only wrote colour that no "
                       "attachment takes), and the main view's z pre-pass loses its R32F colour attachment: the linear depth map (view depth / engine far, cleared to 1) is derived once from the "
                       "hardware depth at AfterDepthPrepass by a full-screen pass (needs reverse-z and a float depth buffer). Cut-out draws keep their texkill pixel shader. Compare = the engine "
                       "still writes the depth map and a compute pass checks the derivation against it (rvk.log `native: depth_z compare`: differing pixels, the largest relative error). Off by default.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "on", "compare"},
        .optionLabels = {"Off", "On", "Compare with the engine's depth map"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeVertexPull = &reg.add({
        .key = "native.vertex_pull",
        .label = "Native frame: vertex pulling",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "S-4 (docs/research/frame-ownership.md 14.12; Full, needs the persistent owned draws): the owned static / skinned draws' vertex shaders read their inputs from the "
                       "mesh arena by reference (a VertexFetch block per record slot, the input assembler's conversions) instead of a vertex input, so the meshes of every vertex "
                       "layout and arena block share one indirect command per pipeline, state and index binding (rvk.log `native: vertex pull (5 s)`: items pulled, indirect "
                       "commands per view before / after). The clip position stays the translated shader's, bit for bit. Off by default.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .needs = {{"native.owned_persistent", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnFoliage = &reg.add({
        .key = "native.own_foliage",
        .label = "Native frame: own foliage (grass thinning)",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "S-5 (docs/research/frame-ownership.md 14.14; Full): the grass's distance thinning tied to lod.view: beyond 35 % x lod.view of the grass range the share of "
                       "blades kept falls to 35 % at the range's end, each blade shrinking to nothing over a short band (no pop). Applied to our converted instance bytes on every "
                       "path (composed, composed late, owned), so the z pre-pass and the colour pass draw the same blades bit for bit. An intentional, small picture change. "
                       "rvk.log `native: own foliage (5 s)`. Off by default.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeTreeWind = &reg.add({
        .key = "native.tree_wind",
        .label = "Native frame: SpeedTree wind by compute (RT scene)",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "F4-5 (docs/research/frame-ownership.md 14.17; Full): the ray tracing scene's animated SpeedTrees (RT shadows' wind, GI foliage) take their swaying "
                       "positions from a compute pass that runs the trees' vertex shader wind (wind matrices, leaf rock / rustle, leaf cards and meshes) over each instance's "
                       "vertex buffer instead of a vertex shader capture of the composed draw, so the trees stay owned (native.owned_trees) with ray tracing on. Compare = the "
                       "captures of the composed draws too, both compared on the GPU (rvk.log `rt: tree wind (5 s)`: max error in world units). Off by default.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "on", "compare"},
        .optionLabels = {"Off", "On", "Compare with the vertex captures"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeBoneMotion = &reg.add({
        .key = "native.bone_motion",
        .label = "Native frame: bone-history motion",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Debug, Full (docs/research/frame-ownership.md 14.13; needs native.owned_actors): the bone table keeps each actor part's previous-frame image, and the skinned "
                       "solid draws' vertex shader runs a second time on it for the previous world position the G-buffer's motion needs, so per-object motion (FSR, frame "
                       "generation) takes no vertex capture of the characters and their owned solid draws stay owned. The clip position is the translated shader's, bit for bit. "
                       "Compare = the captures as before plus a GPU compare of both previous positions (rvk.log `native: bone motion compare (5 s)`, Vulkan). Restart. Off by default.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "on", "compare"},
        .optionLabels = {"Off", "On", "Compare with the vertex captures"},
        .flags = Flag::NoAB | Flag::Debug | Flag::Restart,  // (the bone table's history doubles its buffer, made once)
        .needs = {{"native.owned_actors", "true", false, true}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeRigidMotion = &reg.add({
        .key = "native.rigid_motion",
        .label = "Native frame: rigid-object history motion",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Debug, Full (docs/research/frame-ownership.md 14.15 b; needs native.bone_motion = on, acts with upscale.object_motion = all): the object table keeps each "
                       "record's previous-frame image, and the static meshes' main solid draws whose world rows come from the object table take the bone-motion vertex "
                       "forms (the second run reads the previous record), so moving rigid objects get their per-object motion without a vertex capture and their owned "
                       "draws stay owned. Restart. Off by default.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug | Flag::Restart,  // (the history doubles the object table's buffer, made once)
        .needs = {{"native.bone_motion", "off", true, false}},
        .page = "Debug", .section = "Native frame",
    });
    r.nativeOwnPost = &reg.add({
        .key = "native.own_post",
        .label = "Native frame: post chain on own shaders",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "S-7 (docs/research/frame-ownership.md 14.8; Full, needs native.scene_ops with the post group): the engine's luminance chain (eight "
                       "draws: LogLuminance, DownSample x5, AdaptiveLiminance, the AvgLuminance copy) is one compute dispatch on our own shader that writes "
                       "the two 1x1 adapted-luminance images. On = the units are not drawn; compare = they are drawn and the kernel's result is checked "
                       "against the engine's (rvk.log `native: own post`, `luminance differs` lines must stay 0). S-7b (14.8b): the bloom chain (BrightPass, GlowH, "
                       "Streak, GlowVAddBloom2: three dispatches) and the DoF middle (BlurH, BlurV, NearCoc, SmallBlur: one dispatch) likewise; the small "
                       "A8R8G8B8 images they write are RGBA8 storage images while On. Off by default.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "on", "compare"},
        .optionLabels = {"Off", "On", "Compare with the engine's units"},
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.nativeCoverage = &reg.add({
        .key = "native.coverage",
        .label = "Native frame: coverage counter",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Counts every draw by who issued it and why the engine's stay (rvk.log native: coverage, README section 16). Always on "
                       "with native.frame_tap or a compare setting; this turns it on in plain Full runs too.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Native frame",
    });
    r.streamStats = &reg.add({
        .key = "debug.stream_stats",
        .label = "Loading statistics",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "1 = times the packed-file reads and FMOD sound creation (rvk.log 'engine: stream:' every 5 s), 2 = also "
                       "compares every buffered read with the game's own read of the same bytes.",
        .type = Type::Int,
        .defaultValue = 0,
        .min = 0,
        .max = 2,
        .flags = Flag::NoAB | Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.prefetchStride = &reg.add({
        .key = "debug.prefetch_stride",
        .label = "Cache-in walk every n-th frame",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "The game walks its whole scene tree every frame to load and unload objects around the camera. Every n-th "
                       "frame (and whenever the camera moved a metre) is enough: objects at the edge of their range appear or "
                       "vanish up to n - 1 frames later. 1 = every frame, to compare.",
        .type = Type::Int,
        .defaultValue = 4,
        .min = 1,
        .max = 8,
        .flags = Flag::Debug,
        .page = "Debug", .section = "Renderer internals",
    });
    r.lodView = &reg.add({
        .key = "lod.view",
        .label = "Object draw distance",
        .category = "Detail",
        .description = "How far away small objects (barrels, crates, furniture, effects) are still drawn before they fade out "
                       "and stop popping in. 1 = the game's distances (often only 35-70 m). Farther costs frame rate in "
                       "towns and a little memory.",
        .type = Type::Float,
        .defaultValue = 2,
        .min = 1,
        .max = 3,
        .format = "%.1fx",
        .page = "Display", .section = "Draw distances",
    });
    reg.setDebugSwitch(*r.debugMode);
    if (&reg == &registry()) g_renderer = r;
    return r;
}

const RendererSettings& renderer() { return g_renderer; }

// ---- presets ----------------------------------------------------------------------------------------------------------

const std::vector<Preset>& graphicsPresets() {
    // THE master quality table ("Graphics Quality" in the overlay; docs/research/overlay.md 2.9). Each preset writes only existing
    // quality knobs: the water / sea / sun shadow knobs listed here, plus the RT cost knobs of rtPresets() (Low = rt low, Medium and
    // High = rt medium, Ultra = rt high; what a preset names itself wins over the RT member of the same key). It never switches a
    // feature on that is off by default (shadows.rt, gi.enabled, materials.quality, framegen, upscaling). High = the shipped
    // defaults. Original lists only what turns the enhancements off (the rest does not matter then).
    static const std::vector<Preset> presets = [] {
        std::vector<Preset> p = {
            {"original", "Original",
             {{"water.enabled", "false"}, {"rt.reflections", "off"}, {"shadows.enabled", "false"}, {"shadows.map_size", "game"}}},
            {"low", "Low",
             {{"water.enabled", "true"}, {"water.quality", "low"}, {"water.reflections", "sky"}, {"water.ocean", "gerstner"},
              {"water.ocean_quality", "low"}, {"rt.reflections", "off"}, {"shadows.enabled", "true"}, {"shadows.filter", "low"},
              {"shadows.map_size", "1024"}}},
            {"medium", "Medium",
             {{"water.enabled", "true"}, {"water.quality", "medium"}, {"water.reflections", "ssr"}, {"water.ocean", "fft"},
              {"water.ocean_quality", "medium"}, {"rt.reflections", "medium"}, {"shadows.enabled", "true"}, {"shadows.filter", "low"},
              {"shadows.map_size", "2048"}}},
            {"high", "High",
             {{"water.enabled", "true"}, {"water.quality", "high"}, {"water.reflections", "ssr"}, {"water.ocean", "fft"},
              {"water.ocean_quality", "high"}, {"rt.reflections", "high"}, {"shadows.enabled", "true"}, {"shadows.filter", "low"},
              {"shadows.map_size", "2048"}}},
            {"ultra", "Ultra",
             {{"water.enabled", "true"}, {"water.quality", "high"}, {"water.reflections", "ssr"}, {"water.ocean", "fft"},
              {"water.ocean_quality", "high"}, {"rt.reflections", "high"}, {"shadows.enabled", "true"}, {"shadows.filter", "high"},
              {"shadows.map_size", "4096"}}},
            // Integrated GPUs (a Radeon 680M and the like; docs/research/frame-ownership.md 14.11 has the cost model). The one preset that switches
            // something on that is off by default (FSR); it is chosen by itself only on integrated graphics (applyIntegratedDefaults).
            {"igpu", "Integrated",
             {{"water.enabled", "true"}, {"water.quality", "low"}, {"water.reflections", "ssr"}, {"water.ocean", "gerstner"},
              {"water.ocean_quality", "low"}, {"rt.reflections", "off"}, {"shadows.enabled", "true"}, {"shadows.filter", "low"},
              {"shadows.map_size", "1024"}, {"shadows.range", "60"}, {"gi.enabled", "false"}, {"materials.quality", "medium"},
              {"lighting.light_shadows", "nearby"}, {"upscale.mode", "quality"}, {"lod.trees", "1.5"}, {"lod.meshes", "1.5"},
              {"lod.view", "1.5"}}},
        };
        const auto& rt = rtPresets();  // low, medium, high
        const std::size_t member[6] = {0, 0, 1, 1, 2, 0};  // per preset: its rtPresets() index (Original takes none)
        for (std::size_t i = 1; i < p.size(); ++i) {
            for (const auto& kv : rt[member[i]].values) {
                const bool named = std::ranges::any_of(p[i].values, [&](const auto& e) { return e.first == kv.first; });
                if (!named) p[i].values.push_back(kv);
            }
        }
        return p;
    }();
    return presets;
}

const std::vector<Preset>& rtPresets() {
    // Medium = the defaults of every member (a fresh install matches it; the ladder stays off: measured no gain, docs/research/rt-lod.md); Low is the cheap end: the ladder starts at 0.3 x its base
    // distances, a memory limit, fewer rays; High has the ladder off and the most rays (docs/research/rt-lod.md)
    static const std::vector<Preset> presets = {
        {"low", "Low",
         {{"rt.lod_scale", "0.3"}, {"rt.memory_budget", "384"}, {"shadows.rt_quality", "low"}, {"shadows.rt_range", "15"}, {"lighting.light_shadow_quality", "low"},
          {"rt.reflections", "low"}, {"gi.quality", "low"}, {"gi.screen", "low"}, {"materials.rt_reflections", "low"}, {"materials.rt_range", "60"}}},
        {"medium", "Medium",
         {{"rt.lod_scale", "0"}, {"rt.memory_budget", "0"}, {"shadows.rt_quality", "medium"}, {"shadows.rt_range", "0"}, {"lighting.light_shadow_quality", "medium"},
          {"rt.reflections", "high"}, {"gi.quality", "medium"}, {"gi.screen", "medium"}, {"materials.rt_reflections", "high"}, {"materials.rt_range", "150"}}},
        {"high", "High",
         {{"rt.lod_scale", "0"}, {"rt.memory_budget", "0"}, {"shadows.rt_quality", "high"}, {"shadows.rt_range", "0"}, {"lighting.light_shadow_quality", "high"},
          {"rt.reflections", "high"}, {"gi.quality", "high"}, {"gi.screen", "high"}, {"materials.rt_reflections", "high"}, {"materials.rt_range", "300"}}},
    };
    return presets;
}

int matchRtPreset(const Registry& reg) {
    const auto& presets = rtPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const bool all = std::ranges::all_of(presets[i].values, [&](const auto& kv) {
            const Setting* s = reg.find(kv.first);
            if (s != nullptr && (s->spec().flags & Flag::Hidden) != 0) return true;  // members the player cannot see do not decide "custom"
            double v = 0.0;
            return s != nullptr && Registry::parseText(s->spec(), kv.second, v) && s->value() == v;
        });
        if (all) return static_cast<int>(i);
    }
    return -1;
}

bool syncRtPreset(Registry& reg) {
    Setting* preset = reg.find("rt.preset");
    if (preset == nullptr) return false;
    const int m = matchRtPreset(reg);
    const std::string want = m < 0 ? "custom" : rtPresets()[static_cast<std::size_t>(m)].name;
    if (reg.text(*preset) == want) return false;
    reg.setText("rt.preset", want);
    return true;
}

std::vector<std::string> applyIntegratedDefaults(Registry& reg) {
    std::vector<std::string> changes;
    Setting* done = reg.find("system.igpu_preset_done");
    if (done == nullptr || done->asBool()) return changes;
    const auto& presets = graphicsPresets();
    const auto it = std::ranges::find(presets, std::string("igpu"), &Preset::name);
    if (it != presets.end()) {
        for (const auto& [key, text] : it->values) {
            Setting* s = reg.find(key);
            double v = 0.0;
            if (s == nullptr || s->locked() || !s->isDefault() || !Registry::parseText(s->spec(), text, v) || s->value() == v) continue;
            const std::string before = reg.text(*s);
            if (reg.setText(key, text) == Result::Ok) changes.push_back(key + ": " + before + " -> " + reg.text(*s));
        }
    }
    reg.set(*done, 1);
    return changes;
}

void applyPreset(Registry& reg, const Preset& preset) {
    for (const auto& [key, text] : preset.values) reg.setText(key, text);
}

int matchPreset(const Registry& reg) {
    const auto& presets = graphicsPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const bool all = std::ranges::all_of(presets[i].values, [&](const auto& kv) {
            const Setting* s = reg.find(kv.first);
            double v = 0.0;
            return s != nullptr && Registry::parseText(s->spec(), kv.second, v) && s->value() == v;
        });
        if (all) return static_cast<int>(i);
    }
    return -1;
}

const std::vector<std::string>& pageOrder() {
    static const std::vector<std::string> pages = {kPageDisplay, kPageLighting, kPageRayTracing, kPageWater,
                                                   kPageImage,   kPageSystem,     kPageDebug};
    return pages;
}

Tier tierOf(const Spec& spec) {
    if ((spec.flags & Flag::Hidden) != 0) return Tier::Hidden;
    if ((spec.flags & Flag::Debug) != 0) return Tier::Debug;
    if ((spec.flags & Flag::Advanced) != 0) return Tier::Advanced;
    return Tier::Basic;
}

std::string pageOf(const Spec& spec) {
    if (!spec.page.empty()) return spec.page;
    if (spec.tab == kTabDisplay) return kPageDisplay;
    if (spec.tab == kTabSystem) return kPageSystem;
    if (spec.tab == kTabDebug) return kPageDebug;
    return kPageLighting;
}

std::string sectionOf(const Spec& spec) { return spec.section.empty() ? spec.category : spec.section; }

std::size_t resetPage(Registry& reg, std::string_view page) {
    std::size_t n = 0;
    for (Setting* s : reg.all()) {
        if (pageOf(s->spec()) != page || s->isDefault() || s->locked()) continue;
        reg.resetToDefault(*s);
        ++n;
    }
    return n;
}

std::size_t resetTab(Registry& reg, std::string_view tab) {
    std::size_t n = 0;
    for (Setting* s : reg.all()) {
        if (s->spec().tab != tab || s->isDefault() || s->locked()) continue;
        reg.resetToDefault(*s);
        ++n;
    }
    return n;
}

}  // namespace rvk::settings
