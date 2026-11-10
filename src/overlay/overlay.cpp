// The in-game overlay: ImGui (vcpkg imgui + the Win32 backend, third_party/imgui_vulkan for Vulkan), input filter, settings
// UI, A/B comparison, performance panel. docs/research/overlay.md. The ImGui setup follows GD3D11's ImGuiShim::InitVulkan
// (D3D11Engine/ImGuiShim.cpp, same author as this repository): dynamic rendering, own descriptor pool, no ini file.

#include "gpuscene/owned_compact.h"
#include "overlay/overlay.h"
#include "enh/material_pack.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>

#include <imgui.h>
#include <imgui_internal.h>  // ClosePopupsExceptModals
#include <imgui_impl_win32.h>
#include <spdlog/spdlog.h>

#include "core/perf_window.h"
#include "core/settings.h"
#include "engine/image_effects.h"
#include "engine/menu_state.h"
#include "overlay/keys.h"
#include "engine/window_mode.h"

// declared inside "#if 0" in imgui_impl_win32.h (the backend leaves the declaration to the user)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace rvk::overlay {
namespace {

std::atomic<Overlay*> g_instance{nullptr};
// the game's window procedure (kept for the process lifetime: another subclass may sit on top of ours)
WNDPROC g_original = nullptr;
bool g_unicode = true;

unsigned hotkeyVk(const settings::Setting* s) {
    if (s == nullptr) return 0;
    const std::string_view n = s->asEnum();
    if (n.size() >= 2 && n[0] == 'f' && n.size() <= 3) {
        int k = 0;
        std::from_chars(n.data() + 1, n.data() + n.size(), k);
        return k >= 1 && k <= 12 ? VK_F1 + static_cast<unsigned>(k - 1) : 0;
    }
    if (n == "insert") return VK_INSERT;
    if (n == "home") return VK_HOME;
    if (n == "end") return VK_END;
    if (n == "page_up") return VK_PRIOR;
    if (n == "page_down") return VK_NEXT;
    if (n == "pause") return VK_PAUSE;
    if (n == "scroll_lock") return VK_SCROLL;
    return 0;
}

bool isKeyboardMessage(UINT msg) {
    return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR ||
           msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR || msg == WM_UNICHAR;
}

bool isMouseMessage(UINT msg) { return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT; }

// the virtual key of a mouse button message (0 for others)
unsigned mouseButtonVk(UINT msg, WPARAM wp) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return VK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return VK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: return GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    default: return 0;
    }
}
bool isButtonUp(UINT msg) { return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    for (;;) {
        s = trim(s);
        if (s.empty()) return out;
        const auto sp = s.find_first_of(" \t");
        out.push_back(s.substr(0, sp));
        if (sp == std::string_view::npos) return out;
        s.remove_prefix(sp);
    }
}


const char* sideName(bool b) { return b ? "B" : "A"; }

}  // namespace

Overlay* Overlay::instance() { return g_instance.load(); }

// A copy of the frame's draw lists (the clones are ours; the textures stay the atlas's): drawn later, once per frame the swapchain shows
struct Overlay::Snapshot {
    ImDrawData data;
    std::vector<ImDrawList*> lists;
    void clear() {
        for (ImDrawList* l : lists) IM_DELETE(l);
        lists.clear();
        data.Clear();
    }
    void take(const ImDrawData& dd) {
        clear();
        data = dd;
        for (int i = 0; i < data.CmdLists.Size; ++i) {
            ImDrawList* c = data.CmdLists[i]->CloneOutput();
            lists.push_back(c);
            data.CmdLists[i] = c;
        }
        data.Textures = nullptr;
    }
    ~Snapshot() { clear(); }
};

Overlay::Overlay() = default;
Overlay::~Overlay() { shutdown(); }

bool Overlay::init(d3d9::Device& device, HWND__* window, const Options& options) {
    device_ = &device;
    window_ = window;
    options_ = options;
    if (window == nullptr || !IsWindow(window)) {
        spdlog::warn("overlay: no game window");
        return false;
    }
    for (const auto& c : options.commands) {
        const auto colon = c.find(':');
        std::uint64_t frame = 0;
        std::uint32_t load = 0;
        std::string_view head = trim(std::string_view(c).substr(0, colon));
        // "load<k>+<n>": n presents after the k-th loading screen ended (k = 1 when left out): a schedule relative to the world of a savegame load
        // (scripts/perf-ab.ps1), whatever the loading screens' frame counts were
        // "freeze+<n>": n presents after the game time was first frozen (rvkcompare's spots: risen.debug.time_freeze)
        bool headOk = true;
        if (head.starts_with("freeze+")) {
            head.remove_prefix(7);
            load = kFreeze;
        } else if (head.starts_with("load")) {
            head.remove_prefix(4);
            load = 1;
            const auto plus = head.find('+');
            if (plus == std::string_view::npos) headOk = false;
            else {
                if (plus > 0) {
                    const auto [pk, ek] = std::from_chars(head.data(), head.data() + plus, load);
                    headOk = ek == std::errc() && pk == head.data() + plus && load > 0;
                }
                head.remove_prefix(plus + 1);
            }
        }
        const auto [p, ec] = std::from_chars(head.data(), head.data() + head.size(), frame);
        if (colon == std::string::npos || !headOk || ec != std::errc() || p != head.data() + head.size() || (frame == 0 && load == 0)) {
            spdlog::warn("overlay: [debug] commands: '{}' is not \"<frame>: <command>\", \"load<k>+<n>: <command>\" or \"freeze+<n>: <command>\"", c);
            continue;
        }
        commands_.push_back({frame, std::string(trim(std::string_view(c).substr(colon + 1))), load});
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    initTheme();

    if (!ImGui_ImplWin32_Init(window)) {
        spdlog::error("overlay: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }
    const VkFormat colorFormat = device.overlayFormat();
    rhi::ImGuiBackendDesc bd;
    bd.colorFormat = colorFormat;
    bd.minImageCount = std::max(2u, device.presentMinImageCount());
    bd.imageCount = std::max(bd.minImageCount, std::max(device.presentImageCount(), 8u));  // frame generation draws it for generated frames too
    backend_ = device.game().createImGuiBackend();
    if (!backend_ || !backend_->init(bd)) {
        spdlog::error("overlay: the UI renderer backend failed to initialise");
        backend_.reset();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    imguiVulkan_ = true;

    // subclass the game window (the engine's input arrives through its window procedure: docs/research/overlay.md 3)
    g_unicode = IsWindowUnicode(window) != FALSE;
    g_instance = this;
    const auto proc = reinterpret_cast<LONG_PTR>(&Overlay::wndProc);
    const LONG_PTR previous = g_unicode ? SetWindowLongPtrW(window, GWLP_WNDPROC, proc) : SetWindowLongPtrA(window, GWLP_WNDPROC, proc);
    if (previous == 0) {
        spdlog::error("overlay: subclassing the game window failed (error {})", GetLastError());
        g_instance = nullptr;
        shutdown();
        return false;
    }
    if (previous != proc) {
        g_original = reinterpret_cast<WNDPROC>(previous);
        originalProc_ = reinterpret_cast<void*>(previous);
    }
    initialised_ = true;
    lastB_ = settings::registry().bActive();
    const auto& r = settings::renderer();
    spdlog::info("overlay: ready ({} window, colour format {}, {}; hotkeys: toggle {}, A/B {}); {} scripted commands",
                 g_unicode ? "Unicode" : "ANSI", static_cast<int>(colorFormat),
                 options.intoBackBuffer ? "drawn into the back buffer: screenshots include it" : "drawn after the back-buffer copy",
                 r.toggleKey != nullptr ? r.toggleKey->asEnum() : "?", r.abKey != nullptr ? r.abKey->asEnum() : "?",
                 commands_.size());
    return true;
}

void Overlay::shutdown() {
    {
        const std::lock_guard lock(drawMutex_);
        prepared_.store(false, std::memory_order_release);
        if (imguiVulkan_) snap_.reset();  // the clones belong to ImGui's allocator, while its context lives
    }
    if (initialised_ && window_ != nullptr && IsWindow(window_)) {
        // remove the subclass if nobody subclassed on top of us; otherwise our procedure stays (it forwards when g_instance is null)
        const auto current = g_unicode ? GetWindowLongPtrW(window_, GWLP_WNDPROC) : GetWindowLongPtrA(window_, GWLP_WNDPROC);
        if (current == reinterpret_cast<LONG_PTR>(&Overlay::wndProc) && originalProc_ != nullptr) {
            if (g_unicode) {
                SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProc_));
            } else {
                SetWindowLongPtrA(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProc_));
            }
        }
    }
    Overlay* self = this;
    g_instance.compare_exchange_strong(self, nullptr);
    if (imguiVulkan_) {
        if (backend_) backend_->shutdown();
        backend_.reset();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiVulkan_ = false;
    }
    initialised_ = false;
}

void Overlay::setOpen(bool open) {
    if (open_.exchange(open) != open) {
        openChanged_ = true;
        if (!open) closePopups_ = true;
        spdlog::info("overlay: {}{}", open ? "opened" : "closed",
                     open ? std::string() : std::format(" ({} input messages kept from the game)", blocked_.exchange(0)));
    }
}

// ---- input ----------------------------------------------------------------------------------------------------------

LRESULT CALLBACK Overlay::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = g_instance.load();
    if (o == nullptr) {
        return g_unicode ? CallWindowProcW(g_original, hwnd, msg, wp, lp) : CallWindowProcA(g_original, hwnd, msg, wp, lp);
    }
    return o->handleMessage(hwnd, msg, wp, lp);
}

LRESULT Overlay::callGame(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return g_unicode ? CallWindowProcW(g_original, hwnd, msg, wp, lp) : CallWindowProcA(g_original, hwnd, msg, wp, lp);
}

LRESULT Overlay::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    wndProcThread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    const unsigned vk = static_cast<unsigned>(wp & 0xff);
    if (keyDown || keyUp) {
        // the hotkeys never reach the game (F10 / F11 are unbound in Risen: docs/research/overlay.md 3.3)
        const auto& r = settings::renderer();
        const unsigned toggle = hotkeyVk(r.toggleKey);
        // the A/B comparison is a debug-mode tool: outside it the game gets the key
        const unsigned ab = settings::registry().debugMode() ? hotkeyVk(r.abKey) : 0;
        const bool repeat = (lp & (1 << 30)) != 0;
        if (vk != 0 && vk == toggle) {
            if (keyDown && !repeat) setOpen(!isOpen());
            return 0;
        }
        if (vk != 0 && vk == ab) {
            if (keyDown && !repeat) {
                settings::registry().flipAB();
                spdlog::info("overlay: A/B hotkey: side {} active", sideName(settings::registry().bActive()));
            }
            return 0;
        }
    }
    const unsigned button = mouseButtonVk(msg, wp);
    if (!isOpen()) {
        // closed: the game gets everything; remember what it saw pressed
        if (keyDown) gameKeysDown_.set(vk);
        if (keyUp) gameKeysDown_.reset(vk);
        if (button != 0) {
            if (isButtonUp(msg)) gameKeysDown_.reset(button); else gameKeysDown_.set(button);
        }
        return callGame(hwnd, msg, wp, lp);
    }
    if (isKeyboardMessage(msg) || isMouseMessage(msg)) {
        {
            const std::lock_guard lock(msgMutex_);
            if (msg != WM_INPUT && messages_.size() < 4096) messages_.push_back({msg, wp, lp});
        }
        // releases of keys / buttons the game saw pressed still reach it (nothing stays held down in the game)
        if ((keyUp && gameKeysDown_.test(vk)) || (button != 0 && isButtonUp(msg) && gameKeysDown_.test(button))) {
            gameKeysDown_.reset(keyUp ? vk : button);
            return callGame(hwnd, msg, wp, lp);
        }
        ++blocked_;
        if (msg == WM_INPUT || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR) {
            // WM_INPUT needs DefWindowProc for its cleanup; system keys keep Alt+F4 / Alt+Tab
            return g_unicode ? DefWindowProcW(hwnd, msg, wp, lp) : DefWindowProcA(hwnd, msg, wp, lp);
        }
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);  // ImGui draws its own cursor
        return TRUE;
    }
    return callGame(hwnd, msg, wp, lp);
}

// ---- commands -------------------------------------------------------------------------------------------------------

void Overlay::onPresent(std::uint64_t frame) {
    frame_ = frame;
    const d3d9::Device::LiveFrame& live = device_->liveFrame();
    intervals_[historyPos_] = live.intervalMs;
    cpu_[historyPos_] = live.cpuMs;
    gpu_[historyPos_] = live.gpuMs;
    historyPos_ = (historyPos_ + 1) % kHistory;
    historyCount_ = std::min(historyCount_ + 1, kHistory);
    keyPoster_.tick(window_, frame);  // releases of "key" chords first (a press stays down for at least one frame)
    // anchored commands ("load<k>+<n>"): the ends of the loading screens, while any waits (the menu state query costs a few microseconds)
    if (std::ranges::any_of(commands_, [](const Command& c) { return c.load != 0; })) {
        const engine::menu::Status ms = engine::menu::query();
        const engine::menu::State st = ms.state;
        if (freezeAt_ == 0 && (ms.appState & 8u) != 0) {
            freezeAt_ = frame;
            spdlog::info("overlay: [debug] commands: time freeze first seen at present {}", frame);
        }
        if (st == engine::menu::State::Loading) sawLoading_ = true;
        else if (st == engine::menu::State::Game && sawLoading_) {
            sawLoading_ = false;
            loadEnds_.push_back(frame);
            spdlog::info("overlay: [debug] commands: loading screen {} ended at present {}", loadEnds_.size(), frame);
        }
    }
    const auto due = [&](const Command& c) {
        if (c.load == 0) return c.frame <= frame;
        if (c.load == kFreeze) return freezeAt_ != 0 && freezeAt_ + c.frame <= frame;
        return loadEnds_.size() >= c.load && loadEnds_[c.load - 1] + c.frame <= frame;
    };
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (due(*it)) {
            std::string out;
            const bool ok = runCommand(it->text, &out);
            spdlog::info("overlay: [debug] command at frame {}: '{}' -> {}{}", frame, it->text, ok ? "" : "FAILED: ", out);
            it = commands_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Overlay::runCommand(std::string_view command, std::string* out) {
    const auto w = words(command);
    const auto fail = [&](std::string why) {
        if (out != nullptr) *out = std::move(why);
        return false;
    };
    const auto ok = [&](std::string what) {
        if (out != nullptr) *out = std::move(what);
        return true;
    };
    if (w.empty()) return fail("empty command");
    auto& reg = settings::registry();
    if (w[0] == "overlay" && w.size() == 3 && w[1] == "tab") {
        // selects a tab of the settings window at its next build (screenshots of each tab)
        selectTab_ = std::string(w[2]);
        return ok("tab " + selectTab_);
    }
    if (w[0] == "overlay" && w.size() == 3 && w[1] == "scroll") {
        // scrolls the settings window to a fraction of its height (0 .. 1) for a few frames (screenshots of a tab's lower part)
        scrollFrac_ = std::strtof(std::string(w[2]).c_str(), nullptr);
        scrollFrames_ = 4;
        return ok("scroll " + std::string(w[2]));
    }
    if (w[0] == "overlay" && w.size() == 2) {
        if (w[1] == "open") setOpen(true);
        else if (w[1] == "close") setOpen(false);
        else if (w[1] == "toggle") setOpen(!isOpen());
        else return fail("overlay open|close|toggle");
        return ok(isOpen() ? "open" : "closed");
    }
    if (w[0] == "set" && w.size() >= 3) {
        const auto valueAt = command.find(w[2], static_cast<std::size_t>(w[1].data() + w[1].size() - command.data()));
        const settings::Result r = reg.setText(w[1], trim(command.substr(valueAt)));
        if (r != settings::Result::Ok) return fail(std::string(settings::toString(r)));
        const settings::Setting* s = reg.find(w[1]);
        return ok(std::format("{} = {}", w[1], reg.text(*s)));
    }
    if (w[0] == "ab" && w.size() == 2) {
        if (w[1] == "flip") reg.flipAB();
        else if (w[1] == "a") reg.setSide(false);
        else if (w[1] == "b") reg.setSide(true);
        else if (w[1] == "copy") reg.copyActiveToOther();
        else if (w[1] == "reset") reg.resetOther();
        else return fail("ab flip|a|b|copy|reset");
        return ok(std::format("side {} active, {} settings differ", sideName(reg.bActive()), reg.abDifferences().size()));
    }
    if (w[0] == "debug" && w.size() == 3 && w[1] == "compact") {
        // F2c-4c A/B inside one run: the owned draws' indirect arguments written by the CPU ("cpu") or compacted by the GPU ("gpu", the default)
        if (w[2] != "cpu" && w[2] != "gpu") return fail("debug compact cpu|gpu");
        gpuscene::ownedcompact().setForceCpu(w[2] == "cpu");
        return ok(std::format("owned draws: arguments by the {}", w[2]));
    }
    if (w[0] == "quit" && w.size() == 1) {
        // ends the game at once (perf-ab.ps1 after its last window: no scenario result, rvk.log flushed); TerminateProcess: no engine shutdown code runs
        spdlog::info("overlay: [debug] command quit: terminating the process");
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0);
        return ok("quit");
    }
    if (w[0] == "perf" && w.size() == 4 && w[1] == "window") {
        // a tagged GPU pass / CPU measurement window of <frames> frames from the next recorded frame (core/perf_window.h; needs [debug] perf)
        std::uint32_t frames = 0;
        std::from_chars(w[2].data(), w[2].data() + w[2].size(), frames);
        std::string why;
        const bool armed = perfwin::arm(frames, w[3], &why);
        return armed ? ok(why) : fail(why);
    }
    if (w[0] == "materials" && w.size() == 2 && w[1] == "reload") {
        // the material pack folders are read again (docs/research/material-pack.md)
        return enh::matpack::requestReload() ? ok("material pack reload started") : fail("no material pack is loaded");
    }
    if (w[0] == "display" && w.size() >= 2) {
        // "display fake <W>x<H>" | "display fake off": the display resolver sees another monitor (docs/research/window-mode.md 10)
        const auto rest = command.substr(static_cast<std::size_t>(w[1].data() - command.data()));
        return engine::window::runCommand(rest, out);
    }
    if (w[0] == "post" && w.size() >= 2) {
        // "post effect <name>" / "post default" / "post overlay <name> [dur [in [out]]]" / "post overlay stop [fade]": the engine's image effects
        // (S-7d test aid: RadialBlur, Wobble, EdgeSmooth, OverlayBlackScreen, ...; engine/image_effects.h)
        const auto rest = command.substr(static_cast<std::size_t>(w[1].data() - command.data()));
        return engine::imagefx::runCommand(rest, out);
    }
    if (w[0] == "window" && w.size() >= 2) {
        // the engine's own mode changes (borderless fullscreen, docs/research/window-mode.md)
        const auto rest = command.substr(static_cast<std::size_t>(w[1].data() - command.data()));
        return engine::window::runCommand(rest, out);
    }
    if (keys::Poster::handles(w[0])) return keyPoster_.run(window_, command, frame_, out);
    if ((w[0] == "move" || w[0] == "click") && w.size() == 3) {
        int x = 0, y = 0;
        std::from_chars(w[1].data(), w[1].data() + w[1].size(), x);
        std::from_chars(w[2].data(), w[2].data() + w[2].size(), y);
        const LPARAM pos = MAKELPARAM(x, y);
        PostMessageW(window_, WM_MOUSEMOVE, 0, pos);
        if (w[0] == "click") {
            PostMessageW(window_, WM_LBUTTONDOWN, MK_LBUTTON, pos);
            PostMessageW(window_, WM_LBUTTONUP, 0, pos);
        }
        return ok(std::format("posted {} at {},{}", w[0], x, y));
    }
    return fail("unknown command");
}

// ---- rendering ------------------------------------------------------------------------------------------------------

bool Overlay::active() {
    if (!initialised_) return false;
    const auto& r = settings::renderer();
    const bool b = settings::registry().bActive();
    const auto now = std::chrono::steady_clock::now();
    if (b != lastB_) {
        lastB_ = b;
        if (r.abBadge == nullptr || r.abBadge->asBool()) flipShownUntil_ = now + std::chrono::milliseconds(1500);
    }
    return isOpen() || now < flipShownUntil_ || (r.fpsCounter != nullptr && r.fpsCounter->asBool()) ||
           (b && r.abBadge != nullptr && r.abBadge->asBool());
}

ImDrawData* Overlay::buildFrame(VkExtent2D extent) {
    if (!initialised_) return nullptr;
    ImGuiIO& io = ImGui::GetIO();
    const bool open = isOpen();
    if (!threadsLogged_ && wndProcThread_.load() != 0) {
        threadsLogged_ = true;
        spdlog::info("overlay: window messages on thread {}, rendering on thread {}", wndProcThread_.load(), GetCurrentThreadId());
    }
    std::vector<Msg> msgs;
    {
        const std::lock_guard lock(msgMutex_);
        msgs.swap(messages_);
    }
    // a combo left open would still be open at the next opening
    const bool closePopups = closePopups_.exchange(false);
    if (openChanged_.exchange(false)) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        if (open) {
            // start from the cursor's current position (no mouse move may follow for a while)
            POINT p{};
            if (GetCursorPos(&p) && ScreenToClient(window_, &p)) io.AddMousePosEvent(static_cast<float>(p.x), static_cast<float>(p.y));
        }
    }
    if (open) {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange);
        for (const auto& m : msgs) ImGui_ImplWin32_WndProcHandler(window_, m.msg, m.wparam, m.lparam);
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    }
    io.MouseDrawCursor = open;
    applyTheme(std::clamp(static_cast<float>(extent.height) / 1000.0f, 0.85f, 2.0f));

    backend_->newFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    ImGui::NewFrame();
    if (closePopups) ImGui::ClosePopupsExceptModals();
    buildUi();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    // texture uploads submit to the queue and wait for it (the backend holds the queue lock meanwhile: the present thread presents)
    if (dd->Textures != nullptr) {
        for (ImTextureData* t : *dd->Textures) {
            if (t->Status != ImTextureStatus_OK) backend_->updateTexture(t);
        }
        dd->Textures = nullptr;
    }
    return dd;
}

void Overlay::record(rhi::CommandList& cmd, VkExtent2D extent) {
    const std::lock_guard lock(drawMutex_);
    ImDrawData* dd = buildFrame(extent);
    if (dd != nullptr) backend_->renderDrawData(dd, cmd);
}

bool Overlay::prepare(VkExtent2D extent) {
    const std::lock_guard lock(drawMutex_);
    if (!snap_) snap_ = std::make_unique<Snapshot>();
    ImDrawData* dd = buildFrame(extent);
    if (dd == nullptr) {
        prepared_.store(false, std::memory_order_release);
        return false;
    }
    snap_->take(*dd);
    prepared_.store(true, std::memory_order_release);
    return true;
}

void Overlay::recordPrepared(rhi::CommandList& cmd, VkExtent2D) {
    const std::lock_guard lock(drawMutex_);
    if (!initialised_ || !snap_ || !snap_->data.Valid) return;
    backend_->renderDrawData(&snap_->data, cmd);
}

void Overlay::buildUi() {
    if (isOpen()) {
        buildSettingsWindow();
    }
    buildBadges();
}

void Overlay::buildBadges() {
    const auto& r = settings::renderer();
    auto& reg = settings::registry();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImGuiWindowFlags badge = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const bool b = reg.bActive();
    if (std::chrono::steady_clock::now() < flipShownUntil_) {
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);
        if (ImGui::Begin("##rvk_ab_flip", nullptr, badge)) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 3.0f);
            ImGui::TextColored(b ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%s", sideName(b));
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextUnformatted(b ? "side B\n(the other settings)" : "side A\n(your settings)");
        }
        ImGui::End();
    } else if (b && !isOpen() && r.abBadge != nullptr && r.abBadge->asBool()) {
        ImGui::SetNextWindowPos(ImVec2(display.x - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.5f);
        if (ImGui::Begin("##rvk_ab_tag", nullptr, badge)) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "B");
            ImGui::PopFont();
        }
        ImGui::End();
    }
    if (!isOpen() && r.fpsCounter != nullptr && r.fpsCounter->asBool()) {
        float sum = 0.0f;
        const std::size_t n = std::min<std::size_t>(historyCount_, 60);
        for (std::size_t i = 0; i < n; ++i) sum += intervals_[(historyPos_ + kHistory - 1 - i) % kHistory];
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.4f);
        if (ImGui::Begin("##rvk_fps", nullptr, badge)) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
            const d3d9::Device::LiveFrame& lf = device_->liveFrame();
            if (lf.outputFps > 0.0f) {
                ImGui::Text("%.0f -> %.0f fps", lf.baseFps, lf.outputFps);  // frame generation: rendered -> shown
            } else {
                ImGui::Text("%.0f fps", n > 0 && sum > 0.0f ? 1000.0f * static_cast<float>(n) / sum : 0.0f);
            }
            ImGui::PopFont();
        }
        ImGui::End();
    }
}

}  // namespace rvk::overlay
