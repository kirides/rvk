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
        "# Risen Enhanced Graphics: player settings. The in-game overlay (F10 by default) writes this file.\n"
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
    RendererSettings r;
    r.vsync = &reg.add({
        .key = "vulkan.vsync",
        .label = "Vertical sync",
        .category = "Display",
        .description = "Wait for the display's refresh before showing a frame (no tearing). \"Game option\" follows the "
                       "game's own video setting.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"engine", "on", "off"},
        .optionLabels = {"Game option", "On", "Off"},
    });
    r.maxFps = &reg.add({
        .key = "vulkan.max_fps",
        .label = "Frame rate limit",
        .category = "Display",
        .description = "Upper limit of frames per second (0 = none beyond vertical sync). Below the refresh rate with "
                       "vertical sync on, frames stay locked to the display's refresh (e.g. 60 on a 120 Hz display).",
        .type = Type::Float,
        .defaultValue = 0,
        .min = 0,
        .max = 500,
        .format = "%.0f fps",
    });
    r.adapter = &reg.add({
        .key = "vulkan.adapter",
        .label = "GPU index",
        .category = "Display",
        .description = "Vulkan physical device to render on (-1 = the best discrete GPU). The list is in bin\\rvk\\rvk.log "
                       "(\"probe\").",
        .type = Type::Int,
        .defaultValue = -1,
        .min = -1,
        .max = 15,
        .flags = Flag::Restart,
    });
    r.toggleKey = &reg.add({
        .key = "overlay.toggle_key",
        .label = "Open / close this overlay",
        .category = "Overlay",
        .description = "Hotkey of this overlay. The game never sees it.",
        .type = Type::Enum,
        .defaultValue = keyIndex("f10"),
        .options = hotkeyNames(),
        .optionLabels = keyLabels,
        .flags = Flag::NoAB,
    });
    r.abKey = &reg.add({
        .key = "overlay.ab_key",
        .label = "A/B flip",
        .category = "Overlay",
        .description = "Hotkey that flips between settings side A and side B (also while the overlay is closed). The game "
                       "never sees it.",
        .type = Type::Enum,
        .defaultValue = keyIndex("f11"),
        .options = hotkeyNames(),
        .optionLabels = keyLabels,
        .flags = Flag::NoAB,
    });
    r.fpsCounter = &reg.add({
        .key = "overlay.fps_counter",
        .label = "Frame rate counter",
        .category = "Overlay",
        .description = "Show the frame rate in a corner while the overlay is closed.",
        .type = Type::Bool,
        .defaultValue = 0,
        .flags = Flag::NoAB,
    });
    r.abBadge = &reg.add({
        .key = "overlay.ab_badge",
        .label = "A/B indicator",
        .category = "Overlay",
        .description = "Show which side (A or B) is active after a flip, and a small \"B\" while side B stays active.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB,
    });
    r.tintDraws = &reg.add({
        .key = "debug.tint_draws",
        .label = "Tint draws",
        .category = "Debug",
        .description = "Developer view: colours the scene's draws by what they are (kind: water blue, SpeedTree green, "
                       "skinned red, static meshes grey, instanced cyan, particles orange, unknown magenta, ...) or by their "
                       "pass (solid grey, refracted blue, alpha orange). Off renders exactly as without it.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "kind", "pass"},
        .optionLabels = {"Off", "By kind", "By pass"},
        .flags = Flag::NoAB,
    });
    r.drawRecords = &reg.add({
        .key = "debug.draw_records",
        .label = "Draw records",
        .category = "Debug",
        .description = "Keep a record of every draw of the frame (tags, geometry identity, constants, textures) for later "
                       "features and tools. Off only to measure their cost.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::NoAB | Flag::Hidden,
    });
    r.injectView = &reg.add({
        .key = "debug.inject_view",
        .label = "Injection test view",
        .category = "Debug",
        .description = "Developer view that tests the enhancement plumbing (a GLSL compute pass injected after the opaque "
                       "scene): \"Depth\" colours the scene by distance from the engine's depth map and draws a 5 m grid "
                       "fixed in the world, \"Tint\" cycles the scene's colour over time. Off renders exactly as without it.",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "depth", "tint"},
        .optionLabels = {"Off", "Depth + world grid", "Tint"},
    });
    r.rtAllow = &reg.add({
        .key = "rt.allow",
        .label = "Allow ray tracing",
        .category = "Ray tracing",
        .description = "Turn on the GPU's ray tracing support (acceleration structures + ray query) when the game starts: "
                       "ray-traced enhancements need it. It does not change the image by itself; off keeps the extensions "
                       "disabled (for drivers with problems). Greyed out when the GPU or driver has no ray tracing.",
        .type = Type::Bool,
        .defaultValue = 1,
        .flags = Flag::Restart,
        .requirements = caps::bit(caps::Cap::RayTracingHardware),
        .unmetValue = 0,
    });
    r.waterEnabled = &reg.add({
        .key = "water.enabled",
        .label = "Enhanced water",
        .category = "Water",
        .description = "Replaces the game's water shading: refraction with depth-based colour and absorption, reflections of the "
                       "scene and the sky, sun glints, foam on the sea, in the game's own fog. Each lake, river and swamp keeps "
                       "the colour of its original material. Off renders the game's own water.",
        .type = Type::Bool,
        .defaultValue = 0,
    });
    r.waterQuality = &reg.add({
        .key = "water.quality",
        .label = "Water quality",
        .category = "Water",
        .description = "Detail of the enhanced water: reflection ray steps and the density of the sea's wave grid.",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"low", "medium", "high"},
        .optionLabels = {"Low", "Medium", "High"},
    });
    r.waterReflections = &reg.add({
        .key = "water.reflections",
        .label = "Water reflections",
        .category = "Water",
        .description = "\"Screen space\" reflects what is on screen (shores, ships, trees) and falls back to the sky; \"Sky\" "
                       "reflects the sky only (cheaper).",
        .type = Type::Enum,
        .defaultValue = 1,
        .options = {"sky", "ssr"},
        .optionLabels = {"Sky", "Screen space"},
    });
    r.waterOceanWaves = &reg.add({
        .key = "water.ocean_waves",
        .label = "Sea waves and shore",
        .category = "Water",
        .description = "The open sea gets moving waves, calming down at the coast and around the player, and a real shoreline "
                       "where the water runs up the beach and back instead of the game's painted surf. Lakes, rivers and swamps "
                       "never get waves.",
        .type = Type::Bool,
        .defaultValue = 1,
    });
    r.waterWaveHeight = &reg.add({
        .key = "water.wave_height",
        .label = "Wave height",
        .category = "Water",
        .description = "Height of the sea's waves (1 = about 70 cm for the largest).",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 3,
        .format = "%.2f",
    });
    r.waterMurkiness = &reg.add({
        .key = "water.murkiness",
        .label = "Water murkiness",
        .category = "Water",
        .description = "How quickly the water gets opaque with depth (1 = matched to each water's original look).",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0.25,
        .max = 4,
        .format = "%.2f",
    });
    r.waterReflectionStrength = &reg.add({
        .key = "water.reflection_strength",
        .label = "Reflection strength",
        .category = "Water",
        .description = "Strength of reflections and sun glints on the water (1 = physically based, per water type).",
        .type = Type::Float,
        .defaultValue = 1,
        .min = 0,
        .max = 2,
        .format = "%.2f",
    });
    r.waterDebug = &reg.add({
        .key = "water.debug",
        .label = "Water debug view",
        .category = "Debug",
        .description = "Developer view of the enhanced water: the shore field (distance to the coast), the water depth, the "
                       "surface normals, the reflection alone or the foam (red) over the surf zone (green) and the depth (blue).",
        .type = Type::Enum,
        .defaultValue = 0,
        .options = {"off", "shore_field", "depth", "normals", "reflection", "foam"},
        .optionLabels = {"Off", "Shore field", "Depth", "Normals", "Reflection", "Foam"},
        .flags = Flag::NoAB,
    });
    if (&reg == &registry()) g_renderer = r;
    return r;
}

const RendererSettings& renderer() { return g_renderer; }

}  // namespace rvk::settings
