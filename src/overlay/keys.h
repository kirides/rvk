#pragma once

// Keyboard messages for the overlay's scripted "key" / "keydown" / "keyup" / "type" commands ([debug] commands,
// docs/research/overlay.md, docs/research/debug-mode.md 8). The engine keeps its own key state from the WM_KEYDOWN / WM_KEYUP
// messages of its window (scan code (lParam >> 16) & 0xff plus 0x100 for the extended bit), so chords are posted as the real
// keyboard sends them: modifiers down first, keys released in reverse order, Alt chords as WM_SYSKEY* with the context bit.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct HWND__;

namespace rvk::overlay::keys {

struct Key {
    unsigned vk = 0;     // wParam (VK_CONTROL / VK_MENU / VK_SHIFT for both sides, as Windows sends them)
    unsigned scan = 0;   // set-1 make code (lParam bits 16..23)
    bool extended = false;
    bool ctrl = false;   // the key is a Ctrl key
    bool alt = false;    // the key is an Alt key
};

// "f12", "ctrl+insert", "ctrl_l+h", "alt_r+f12", "num8", "pause", "shift+w": names joined with '+' (case-insensitive).
// Modifier names: ctrl / ctrl_l / lctrl, ctrl_r / rctrl, alt / alt_l / lalt, alt_r / ralt / altgr (AltGr = Ctrl_L + Alt_R), shift /
// shift_l / lshift, shift_r / rshift. Special keys: insert, delete, home, end, page_up / pgup, page_down / pgdn, pause, numlock,
// scroll_lock, num0..num9, num_0..num_9, up / down / left / right, escape / esc, enter, space, tab, backspace, f1..f12,
// oe / ae / ue (the German umlaut keys, by scan code), A-Z, 0-9. nullopt (and `error`) for an unknown name.
std::optional<std::vector<Key>> parseChord(std::string_view text, std::string* error = nullptr);

struct Message {
    unsigned msg = 0;
    std::uintptr_t wparam = 0;
    std::intptr_t lparam = 0;
};

// the messages of pressing the chord's keys in order (each key stays down) / releasing them in reverse order
std::vector<Message> downMessages(const std::vector<Key>& chord);
std::vector<Message> upMessages(const std::vector<Key>& chord);
// WM_CHAR per character (what typing produces; the game's cheat-code scanner reads WM_CHAR)
std::vector<Message> charMessages(std::string_view text);

// The scripted key commands of [debug] commands: "key <chord> [frames]" (down now, up after `frames` frames, default 1: the
// engine's per-frame input processing sees the key held), "keydown <chord>", "keyup <chord>", "type <text>". The overlay runs
// them at presents (Vulkan mode); bootstrap at eCApplication::Draw calls in the other modes (a D3D9 baseline of the same input).
class Poster {
public:
    static bool handles(std::string_view verb) { return verb == "key" || verb == "keydown" || verb == "keyup" || verb == "type"; }
    // posts to `window`; the result / error text in `out`
    bool run(HWND__* window, std::string_view command, std::uint64_t frame, std::string* out);
    // releases the chords that are due at `frame`
    void tick(HWND__* window, std::uint64_t frame);

private:
    struct PendingUp {
        std::uint64_t frame;
        std::vector<Key> chord;
    };
    std::vector<PendingUp> pendingUps_;
};

}  // namespace rvk::overlay::keys
