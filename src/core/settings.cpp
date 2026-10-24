#include "core/settings.h"

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
    if (s->spec_.requirements != 0) {
        s->unmetReason_ = resolve(s->spec_, caps_);
        s->unmet_ = !s->unmetReason_.empty();
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

void Registry::resolveLocked(Setting& s) {
    const double before = s.effective();
    s.unmetReason_ = resolve(s.spec_, caps_);
    s.unmet_ = !s.unmetReason_.empty();
    if (s.effective() != before) changedLocked(s);
}

void Registry::setCapabilities(const caps::Report& report) {
    std::vector<std::string> notes;
    {
        const std::lock_guard lock(mutex_);
        caps_ = report;
        for (auto& s : settings_) {
            if (s->spec_.requirements == 0) continue;
            resolveLocked(*s);
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
    for (const auto& leaf : leaves) {
        const bool sideB = leaf.starts_with("ab.");
        const std::string key = sideB ? leaf.substr(3) : leaf;
        Setting* s = nullptr;
        for (auto& x : settings_) {
            if (x->spec_.key == key) s = x.get();
        }
        if (s == nullptr) {
            warn(std::format("unknown setting '{}' ignored", leaf));
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
    });
    r.maxFps = &reg.add({
        .key = "vulkan.max_fps",
        .label = "Frame rate limit",
        .category = "Display",
        .tab = kTabDisplay,
        .description = "The highest frame rate the game runs at. A limit saves power and keeps the graphics card cooler. With "
                       "vertical sync on, a limit below the monitor's refresh rate keeps frames in step with the monitor "
                       "(e.g. 60 on a 120 Hz monitor). Loading screens are never limited, so loading stays fast.",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 500,
        .format = "%.0f fps",
        .autoLabel = "Unlimited",
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
    });

    // ---- System tab ----
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
        .flags = Flag::NoAB,
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
    });
    r.adapter = &reg.add({
        .key = "vulkan.adapter",
        .label = "Graphics card",
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
    });
    r.exclusiveAcquire = &reg.add({
        .key = "vulkan.exclusive_acquire",
        .label = "Vulkan exclusive mode",
        .category = "Hardware",
        .tab = kTabSystem,
        .description = "Exclusive fullscreen: also take Vulkan's exclusive mode (VK_EXT_full_screen_exclusive) on top of the "
                       "display mode change. Diagnostics (docs/research/window-mode.md 8.3).",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Hidden,
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
    });
    r.drawRecords = &reg.add({
        .key = "debug.draw_records",
        .label = "Draw records",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Keep a record of every draw of the frame (tags, geometry identity, constants, textures) for later "
                       "features and tools. Off only to measure their cost.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Hidden,
    });
    r.paceLoading = &reg.add({
        .key = "debug.pace_loading",
        .label = "Pace loading screens",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Loading screens wait for the frame rate limit / vsync like other frames (the behaviour before "
                       "2026-10-23: each wait lengthens the load). Only to measure the load-time penalty.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB | Flag::Hidden,
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
        .options = {"off", "instances", "depth", "normals", "albedo", "albedo_split", "albedo_opaque", "uv"},
        .optionLabels = {"Off", "Instances", "Depth check", "Normals", "Albedo", "Albedo (split)", "Albedo (cut-outs solid)", "UV"},
        .flags = Flag::NoAB | Flag::Debug,
        .requirements = caps::bit(caps::Cap::RayQuery),
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
        .flags = Flag::NoAB | Flag::Hidden,
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
        .flags = Flag::NoAB | Flag::Hidden,
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
    });
    r.rtReflections = &reg.add({
        .key = "rt.reflections",
        .label = "Ray-traced reflections",
        .category = "Water",
        .description = "Reflections on the water traced through the world: cliffs, trees, ships and houses are reflected even "
                       "when they are not on screen, with their own textures and shadows. Low is the fastest, High looks "
                       "best. Off: the water reflects only what is on screen. Needs a graphics card with ray tracing.",
        .type = Type::Enum,
        .defaultValue = 3,
        .options = {"off", "low", "medium", "high"},
        .optionLabels = {"Off", "Low", "Medium", "High"},
        .requirements = caps::bit(caps::Cap::RayQuery),
        .unmetValue = 0,
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
    });
    r.waterMurkiness = &reg.add({
        .key = "water.murkiness",
        .label = "Water murkiness",
        .category = "Water",
        .description = "How quickly the water gets cloudy with depth (1 = like each water's original look).",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0.25,
        .max = 4,
        .format = "%.2f",
        .flags = Flag::Advanced,
    });
    r.waterReflectionStrength = &reg.add({
        .key = "water.reflection_strength",
        .label = "Reflection strength",
        .category = "Water",
        .description = "Brightness of the reflections and sun glints on the water (1 = realistic).",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 2,
        .format = "%.2f",
        .flags = Flag::Advanced,
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
    });
    r.shadowsSoftness = &reg.add({
        .key = "shadows.softness",
        .label = "Shadow softness",
        .category = "Shadows",
        .description = "Width of the soft shadow edges.",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0.5,
        .max = 3,
        .format = "%.2f",
        .flags = Flag::Advanced,
    });
    r.shadowsBlend = &reg.add({
        .key = "shadows.cascade_blend",
        .label = "Shadow detail blending",
        .category = "Shadows",
        .description = "How gently the sharp shadows near the player blend into the coarser distant ones (0 = a hard line, "
                       "like the game).",
        .type = Type::Float,
        .defaultValue = 0.2,
        .min = 0,
        .max = 0.5,
        .format = "%.2f",
        .flags = Flag::Advanced,
    });
    r.shadowsFade = &reg.add({
        .key = "shadows.fade",
        .label = "Shadow fade-out",
        .category = "Shadows",
        .description = "How gradually shadows fade out towards the end of the shadow distance.",
        .type = Type::Float,
        .defaultValue = 0.35,
        .min = 0.05,
        .max = 0.8,
        .format = "%.2f",
        .flags = Flag::Advanced,
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
    });
    r.shadowsSplit = &reg.add({
        .key = "shadows.split",
        .label = "Shadow detail distribution",
        .category = "Shadows",
        .description = "Where the shadow detail goes when the shadow distance is changed: 0 keeps the most detail close to "
                       "the player, 1 spreads it evenly.",
        .type = Type::Float,
        .defaultValue = 0.4,
        .min = 0,
        .max = 1,
        .format = "%.2f",
        .flags = Flag::Advanced,
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
        .options = {"off", "cascades", "cascade_term", "terrain_term"},
        .optionLabels = {"Off", "Cascades", "Cascade shadows only", "Terrain shadows only"},
        .flags = Flag::NoAB | Flag::Debug,
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
    });
    r.giCharacters = &reg.add({
        .key = "gi.characters",
        .label = "Characters in ray tracing",
        .category = "Lighting",
        .description = "The hero, people and monsters block and bounce the traced light (and appear in ray-traced "
                       "reflections).",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Advanced,
    });
    r.giScreen = &reg.add({
        .key = "gi.screen",
        .label = "Lighting detail",
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
    });
    r.giDebug = &reg.add({
        .key = "gi.debug",
        .label = "GI debug view",
        .category = "Debug views",
        .tab = kTabDebug,
        .description = "Developer view of the ray-traced lighting: \"Irradiance\" shows the probes' light on a white surface "
                       "(geometric normal), \"Cascades\" the probe cascade each pixel uses (red, green, blue; dark red = no "
                       "probes: the game's ambient).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "irradiance", "cascades"},
        .optionLabels = {"Off", "Irradiance", "Cascades"},
        .flags = Flag::NoAB | Flag::Debug,
        .requirements = caps::bit(caps::Cap::RayQuery),
    });

    r.aoEnabled = &reg.add({
        .key = "ao.enabled",
        .label = "Ambient occlusion",
        .category = "Lighting",
        .description = "Soft contact shade in corners, creases, under furniture and where objects meet the ground (on the "
                       "ambient light; works with and without ray-traced lighting).",
        .type = Type::Bool,
        .defaultValue = 0,
    });
    r.aoMode = &reg.add({
        .key = "ao.mode",
        .label = "Game's ambient occlusion",
        .category = "Lighting",
        .description = "Replace: the game's own ambient occlusion is switched off and only the new one shades (on the ambient "
                       "light only: sunlit edges stay bright). Add: both shade (darker creases).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"replace", "add"},
        .optionLabels = {"Replace", "Add"},
        .flags = Flag::Advanced,
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
    });
    r.aoRadius = &reg.add({
        .key = "ao.radius",
        .label = "Ambient occlusion reach",
        .category = "Lighting",
        .description = "How far (in metres) surfaces shade each other.",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0.25,
        .max = 3,
        .format = "%.2f m",
        .flags = Flag::Advanced,
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
        .options = {"off", "normals", "motion", "ao", "screen_gi"},
        .optionLabels = {"Off", "Normals", "Motion", "AO", "Screen probes (GI factor)"},
        .flags = Flag::NoAB | Flag::Debug,
    });

    // ---- Graphics tab: upscaling / anti-aliasing (enh/upscale.h) ----
    r.upscaleMode = &reg.add({
        .key = "upscale.mode",
        .label = "Anti-aliasing / upscaling",
        .category = "Image",
        .description = "AMD FidelityFX Super Resolution 3.1: a temporal filter that removes jagged edges and shimmer. Native AA "
                       "renders at the screen's resolution; Quality to Ultra performance render the 3D scene at 1/1.5 .. 1/3 of "
                       "it per dimension (faster) and upscale it; the menus and the HUD stay sharp. \"Jitter only\" shakes "
                       "the camera without the filter (developer test).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "native_aa", "quality", "balanced", "performance", "ultra_performance", "jitter_only"},
        .optionLabels = {"Off", "Native AA (FSR)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2x)",
                         "Ultra performance (3x)", "Jitter only (test)"},
    });

    r.upscaleObjectMotion = &reg.add({
        .key = "upscale.object_motion",
        .label = "Motion of characters and objects",
        .category = "Image",
        .description = "Tells FSR how characters (and with \"Characters and objects\" also moving rigid objects such as a drawn "
                       "weapon) really move, so they stay sharp instead of blurring while they animate.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"off", "characters", "all"},
        .optionLabels = {"Off", "Characters", "Characters and objects"},
        .flags = Flag::Advanced,
    });
    r.upscaleReactive = &reg.add({
        .key = "upscale.reactive",
        .label = "Clean up effects",
        .category = "Image",
        .description = "Keeps particles, rain, spells and spray from leaving smeared trails behind them: FSR trusts its history "
                       "less where such effects are drawn.",
        .type = Type::Bool,
        .defaultValue = 1,
    });
    r.upscaleReactiveCutoff = &reg.add({
        .key = "upscale.reactive_cutoff",
        .label = "Effect detection threshold",
        .category = "Image",
        .description = "How different a pixel must look with and without the translucent effects before FSR treats it as an "
                       "effect. Lower catches fainter effects (rain), 0 = every difference counts.",
        .type = Type::Float,
        .defaultValue = 0.08,
        .min = 0,
        .max = 1,
        .format = "%.3f",
        .flags = Flag::Advanced,
    });
    r.upscaleReactiveScale = &reg.add({
        .key = "upscale.reactive_scale",
        .label = "Effect clean-up strength",
        .category = "Image",
        .description = "Multiplies the effect mask: higher trusts the temporal history less where effects are drawn.",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 4,
        .format = "%.2f",
        .flags = Flag::Advanced,
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
    });
    reg.setDebugSwitch(*r.debugMode);
    if (&reg == &registry()) g_renderer = r;
    return r;
}

const RendererSettings& renderer() { return g_renderer; }

// ---- presets ----------------------------------------------------------------------------------------------------------

const std::vector<Preset>& graphicsPresets() {
    // High = the defaults. Original lists only what turns the enhancements off (the rest does not matter then).
    static const std::vector<Preset> presets = {
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
    };
    return presets;
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
