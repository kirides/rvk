// Keyboard messages of the overlay's scripted key commands (overlay/keys.h).

#include "overlay/keys.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace rvk::overlay::keys {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct Named {
    const char* name;
    unsigned vk;
    unsigned scan;  // 0: MapVirtualKeyW
    bool extended;
    bool ctrl;
    bool alt;
};

// scan codes of the keys whose code does not follow from the layout (set 1; extended = the E0 prefix)
constexpr Named kNamed[] = {
    {"ctrl", VK_CONTROL, 0x1d, false, true, false},   {"ctrl_l", VK_CONTROL, 0x1d, false, true, false},
    {"lctrl", VK_CONTROL, 0x1d, false, true, false},  {"ctrl_r", VK_CONTROL, 0x1d, true, true, false},
    {"rctrl", VK_CONTROL, 0x1d, true, true, false},   {"alt", VK_MENU, 0x38, false, false, true},
    {"alt_l", VK_MENU, 0x38, false, false, true},     {"lalt", VK_MENU, 0x38, false, false, true},
    {"alt_r", VK_MENU, 0x38, true, false, true},      {"ralt", VK_MENU, 0x38, true, false, true},
    {"shift", VK_SHIFT, 0x2a, false, false, false},   {"shift_l", VK_SHIFT, 0x2a, false, false, false},
    {"lshift", VK_SHIFT, 0x2a, false, false, false},  {"shift_r", VK_SHIFT, 0x36, false, false, false},
    {"rshift", VK_SHIFT, 0x36, false, false, false},  {"insert", VK_INSERT, 0x52, true, false, false},
    {"delete", VK_DELETE, 0x53, true, false, false},  {"home", VK_HOME, 0x47, true, false, false},
    {"end", VK_END, 0x4f, true, false, false},        {"page_up", VK_PRIOR, 0x49, true, false, false},
    {"pgup", VK_PRIOR, 0x49, true, false, false},     {"page_down", VK_NEXT, 0x51, true, false, false},
    {"pgdn", VK_NEXT, 0x51, true, false, false},      {"up", VK_UP, 0x48, true, false, false},
    {"down", VK_DOWN, 0x50, true, false, false},      {"left", VK_LEFT, 0x4b, true, false, false},
    {"right", VK_RIGHT, 0x4d, true, false, false},    {"pause", VK_PAUSE, 0x45, false, false, false},
    {"numlock", VK_NUMLOCK, 0x45, true, false, false}, {"scroll_lock", VK_SCROLL, 0x46, false, false, false},
    {"escape", VK_ESCAPE, 0x01, false, false, false}, {"esc", VK_ESCAPE, 0x01, false, false, false},
    {"enter", VK_RETURN, 0x1c, false, false, false},  {"space", VK_SPACE, 0x39, false, false, false},
    {"tab", VK_TAB, 0x0f, false, false, false},       {"backspace", VK_BACK, 0x0e, false, false, false},
    {"num0", VK_NUMPAD0, 0x52, false, false, false},  {"num1", VK_NUMPAD1, 0x4f, false, false, false},
    {"num2", VK_NUMPAD2, 0x50, false, false, false},  {"num3", VK_NUMPAD3, 0x51, false, false, false},
    {"num4", VK_NUMPAD4, 0x4b, false, false, false},  {"num5", VK_NUMPAD5, 0x4c, false, false, false},
    {"num6", VK_NUMPAD6, 0x4d, false, false, false},  {"num7", VK_NUMPAD7, 0x47, false, false, false},
    {"num8", VK_NUMPAD8, 0x48, false, false, false},  {"num9", VK_NUMPAD9, 0x49, false, false, false},
    // the German umlaut keys by their position (the engine's OE / AE / UE)
    {"oe", 0, 0x27, false, false, false},             {"ae", 0, 0x28, false, false, false},
    {"ue", 0, 0x1a, false, false, false},
};

std::optional<Key> keyFromName(std::string_view n) {
    std::string name = lower(n);
    if (name.starts_with("num_") && name.size() == 5) name.erase(3, 1);  // num_8 -> num8 (the engine's NUM_8)
    for (const Named& x : kNamed) {
        if (name == x.name) {
            Key k{x.vk, x.scan, x.extended, x.ctrl, x.alt};
            if (k.vk == 0) k.vk = MapVirtualKeyW(k.scan, MAPVK_VSC_TO_VK);
            return k;
        }
    }
    unsigned vk = 0;
    if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) {
        vk = static_cast<unsigned>(std::toupper(static_cast<unsigned char>(name[0])));
    } else if (name.size() >= 2 && name[0] == 'f') {
        int f = 0;
        const auto [p, ec] = std::from_chars(name.data() + 1, name.data() + name.size(), f);
        if (ec == std::errc() && p == name.data() + name.size() && f >= 1 && f <= 12) vk = VK_F1 + static_cast<unsigned>(f - 1);
    }
    if (vk == 0) return std::nullopt;
    return Key{vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xff, false, false, false};
}

std::intptr_t lparam(const Key& k, bool alt, bool up) {
    std::intptr_t lp = 1 | (static_cast<std::intptr_t>(k.scan & 0xff) << 16);
    if (k.extended) lp |= std::intptr_t{1} << 24;
    if (alt) lp |= std::intptr_t{1} << 29;
    if (up) lp |= (std::intptr_t{1} << 30) | (std::intptr_t{1} << 31);
    return lp;
}

}  // namespace

std::optional<std::vector<Key>> parseChord(std::string_view text, std::string* error) {
    std::vector<Key> out;
    while (true) {
        const auto plus = text.find('+');
        const std::string_view part = text.substr(0, plus);
        const auto k = keyFromName(part);
        if (!k) {
            if (error != nullptr) *error = std::format("unknown key name '{}'", part);
            return std::nullopt;
        }
        out.push_back(*k);
        if (plus == std::string_view::npos) break;
        text.remove_prefix(plus + 1);
    }
    return out;
}

std::vector<Message> downMessages(const std::vector<Key>& chord) {
    std::vector<Message> out;
    bool ctrl = false, alt = false;
    for (const Key& k : chord) {
        ctrl |= k.ctrl;
        alt |= k.alt;
        // Alt without Ctrl makes system keys (an AltGr chord = Ctrl + Alt arrives as normal key messages)
        const bool sys = alt && !ctrl;
        out.push_back({sys ? static_cast<unsigned>(WM_SYSKEYDOWN) : static_cast<unsigned>(WM_KEYDOWN), k.vk, lparam(k, alt, false)});
    }
    return out;
}

std::vector<Message> upMessages(const std::vector<Key>& chord) {
    bool ctrl = false, alt = false;
    for (const Key& k : chord) {
        ctrl |= k.ctrl;
        alt |= k.alt;
    }
    std::vector<Message> out;
    for (auto it = chord.rbegin(); it != chord.rend(); ++it) {
        const bool sys = alt && !ctrl;
        out.push_back({sys ? static_cast<unsigned>(WM_SYSKEYUP) : static_cast<unsigned>(WM_KEYUP), it->vk, lparam(*it, alt, true)});
        // the state after this release
        if (it->ctrl) ctrl = std::any_of(chord.begin(), std::prev(it.base()), [](const Key& k) { return k.ctrl; });
        if (it->alt) alt = std::any_of(chord.begin(), std::prev(it.base()), [](const Key& k) { return k.alt; });
    }
    return out;
}

std::vector<Message> charMessages(std::string_view text) {
    std::vector<Message> out;
    for (const char c : text) {
        const auto ch = static_cast<unsigned char>(c);
        const unsigned vkScan = MapVirtualKeyW(LOBYTE(VkKeyScanW(static_cast<wchar_t>(ch))), MAPVK_VK_TO_VSC);
        out.push_back({WM_CHAR, ch, 1 | (static_cast<std::intptr_t>(vkScan & 0xff) << 16)});
    }
    return out;
}

namespace {
void post(HWND window, const std::vector<Message>& messages) {
    for (const auto& m : messages) PostMessageW(window, m.msg, m.wparam, m.lparam);
}
}  // namespace

bool Poster::run(HWND__* window, std::string_view command, std::uint64_t frame, std::string* out) {
    const auto set = [&](std::string text, bool result) {
        if (out != nullptr) *out = std::move(text);
        return result;
    };
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front()))) command.remove_prefix(1);
    const auto sp = command.find_first_of(" \t");
    const std::string_view verb = command.substr(0, sp);
    std::string_view rest = sp == std::string_view::npos ? std::string_view() : command.substr(sp + 1);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.remove_prefix(1);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.remove_suffix(1);
    if (window == nullptr) return set("no game window", false);
    if (verb == "type") {
        if (rest.empty()) return set("type <text>", false);
        post(window, charMessages(rest));
        return set(std::format("posted {} WM_CHAR", rest.size()), true);
    }
    const auto sp2 = rest.find_first_of(" \t");
    const std::string_view chordText = rest.substr(0, sp2);
    std::string_view holdText = sp2 == std::string_view::npos ? std::string_view() : rest.substr(sp2 + 1);
    while (!holdText.empty() && std::isspace(static_cast<unsigned char>(holdText.front()))) holdText.remove_prefix(1);
    if (chordText.empty() || (!holdText.empty() && verb != "key")) return set(std::format("{} <chord>{}", verb, verb == "key" ? " [frames]" : ""), false);
    std::string error;
    const auto chord = parseChord(chordText, &error);
    if (!chord) return set(error, false);
    std::uint64_t hold = 1;
    if (!holdText.empty()) {
        const auto [p, ec] = std::from_chars(holdText.data(), holdText.data() + holdText.size(), hold);
        if (ec != std::errc() || p != holdText.data() + holdText.size()) return set("key <chord> [frames]", false);
    }
    if (verb == "keyup") {
        post(window, upMessages(*chord));
        return set(std::format("released {} keys", chord->size()), true);
    }
    post(window, downMessages(*chord));
    if (verb == "keydown") return set(std::format("pressed {} keys", chord->size()), true);
    if (hold == 0) {
        post(window, upMessages(*chord));
    } else {
        pendingUps_.push_back({frame + hold, *chord});
    }
    return set(std::format("pressed {} keys, released after {} frames", chord->size(), hold), true);
}

void Poster::tick(HWND__* window, std::uint64_t frame) {
    for (auto it = pendingUps_.begin(); it != pendingUps_.end();) {
        if (it->frame <= frame) {
            if (window != nullptr) post(window, upMessages(it->chord));
            it = pendingUps_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace rvk::overlay::keys
