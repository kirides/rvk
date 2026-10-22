// Scripted key messages of the overlay's [debug] commands (src/overlay/keys.*, docs/research/debug-mode.md 8).

#include <windows.h>

#include "overlay/keys.h"
#include "test.h"

using namespace rvk::overlay::keys;

namespace {
unsigned scanOf(const Message& m) { return static_cast<unsigned>((m.lparam >> 16) & 0xff); }
bool extOf(const Message& m) { return ((m.lparam >> 24) & 1) != 0; }
bool upOf(const Message& m) { return ((m.lparam >> 31) & 1) != 0; }
bool altCtx(const Message& m) { return ((m.lparam >> 29) & 1) != 0; }
}  // namespace

TEST(keys_chord_ctrl_insert) {
    const auto c = parseChord("CTRL_L+Insert");
    CHECK(c.has_value() && c->size() == 2);
    const auto down = downMessages(*c);
    CHECK(down.size() == 2);
    CHECK(down[0].msg == WM_KEYDOWN && down[0].wparam == VK_CONTROL && scanOf(down[0]) == 0x1d && !extOf(down[0]));
    CHECK(down[1].msg == WM_KEYDOWN && down[1].wparam == VK_INSERT && scanOf(down[1]) == 0x52 && extOf(down[1]));
    const auto up = upMessages(*c);
    CHECK(up.size() == 2);
    CHECK(up[0].msg == WM_KEYUP && up[0].wparam == VK_INSERT && upOf(up[0]));  // reverse order
    CHECK(up[1].msg == WM_KEYUP && up[1].wparam == VK_CONTROL && upOf(up[1]));
}

TEST(keys_specials) {
    const auto num8 = parseChord("num_8");
    CHECK(num8 && (*num8)[0].vk == VK_NUMPAD8 && (*num8)[0].scan == 0x48 && !(*num8)[0].extended);
    const auto pause = parseChord("pause");
    CHECK(pause && (*pause)[0].vk == VK_PAUSE && (*pause)[0].scan == 0x45 && !(*pause)[0].extended);
    const auto f12 = parseChord("f12");
    CHECK(f12 && (*f12)[0].vk == VK_F12);
    const auto h = parseChord("ctrl+h");
    CHECK(h && h->size() == 2 && (*h)[1].vk == 'H' && (*h)[1].scan != 0);
    std::string error;
    CHECK(!parseChord("ctrl+nokey", &error).has_value());
    CHECK_MSG(error.find("nokey") != std::string::npos, error);
    CHECK(!parseChord("f13").has_value());
}

TEST(keys_alt_chords) {
    // Alt alone: system keys with the context bit
    const auto a = parseChord("alt_l+m");
    const auto down = downMessages(*a);
    CHECK(down[0].msg == WM_SYSKEYDOWN && down[1].msg == WM_SYSKEYDOWN && altCtx(down[1]));
    const auto r = parseChord("alt_r+f12");
    CHECK(r && (*r)[0].extended && downMessages(*r)[1].msg == WM_SYSKEYDOWN);
    // AltGr (Ctrl + Alt): normal key messages
    const auto g = parseChord("ctrl_l+alt_r+f12");
    const auto gd = downMessages(*g);
    CHECK(gd[2].msg == WM_KEYDOWN);
    const auto gu = upMessages(*g);
    CHECK(gu.size() == 3 && gu[0].wparam == VK_F12 && gu[2].wparam == VK_CONTROL);
}

TEST(keys_type) {
    const auto m = charMessages("minsky");
    CHECK(m.size() == 6 && m[0].msg == WM_CHAR && m[0].wparam == 'm' && m[5].wparam == 'y');
}
