// The in-game overlay: ImGui (vcpkg imgui + the Win32 backend, third_party/imgui_vulkan for Vulkan), input filter, settings
// UI, A/B comparison, performance panel. docs/research/overlay.md. The ImGui setup follows GD3D11's ImGuiShim::InitVulkan
// (D3D11Engine/ImGuiShim.cpp, same author as this repository): dynamic rendering, own descriptor pool, no ini file.

#include "overlay/overlay.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

#include <imgui.h>
#include <imgui_internal.h>  // ClosePopupsExceptModals
#include <imgui_impl_win32.h>
#include <spdlog/spdlog.h>

#include "core/settings.h"
#include "overlay/keys.h"
#include "engine/window_mode.h"
#define IMGUI_IMPL_VULKAN_USE_VOLK
#include "imgui_impl_vulkan.h"

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

void checkVk(VkResult r) {
    if (r < 0) spdlog::error("overlay: imgui_impl_vulkan: {}", vk::resultString(r));
}

const char* sideName(bool b) { return b ? "B" : "A"; }

}  // namespace

Overlay* Overlay::instance() { return g_instance.load(); }

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
        const std::string_view head = trim(std::string_view(c).substr(0, colon));
        const auto [p, ec] = std::from_chars(head.data(), head.data() + head.size(), frame);
        if (colon == std::string::npos || ec != std::errc() || p != head.data() + head.size() || frame == 0) {
            spdlog::warn("overlay: [debug] commands: '{}' is not \"<frame>: <command>\"", c);
            continue;
        }
        commands_.push_back({frame, std::string(trim(std::string_view(c).substr(colon + 1)))});
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.97f;
    style.Colors[ImGuiCol_PopupBg].w = 0.98f;
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const std::filesystem::path font = std::filesystem::path(winDir) / L"Fonts" / L"segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::exists(font, ec)) {
        const std::u8string u8 = font.u8string();
        io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(u8.c_str()), 18.0f);
    } else {
        io.Fonts->AddFontDefault();
    }
    style.FontSizeBase = 18.0f;

    if (!ImGui_ImplWin32_Init(window)) {
        spdlog::error("overlay: ImGui_ImplWin32_Init failed");
        ImGui::DestroyContext();
        return false;
    }
    vk::Context& ctx = device.ctx();
    static VkFormat colorFormat;  // only read inside ImGui_ImplVulkan_Init
    colorFormat = device.overlayFormat();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = ctx.instance();
    ii.PhysicalDevice = ctx.physicalDevice();
    ii.Device = ctx.device();
    ii.QueueFamily = ctx.queueFamily();
    ii.Queue = ctx.queue();
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 8;
    ii.MinImageCount = std::max(2u, device.swapchain().minImageCount());
    ii.ImageCount = std::max(ii.MinImageCount, std::max(device.swapchain().imageCount(), static_cast<std::uint32_t>(vk::Frames::kInFlight) + 1));
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    ii.CheckVkResultFn = &checkVk;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        spdlog::error("overlay: ImGui_ImplVulkan_Init failed");
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
        ImGui_ImplVulkan_Shutdown();
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
        const unsigned ab = hotkeyVk(r.abKey);
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
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->frame <= frame) {
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

void Overlay::record(VkCommandBuffer cmd, VkExtent2D extent) {
    if (!initialised_) return;
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
    ImGui::GetStyle().FontScaleMain = std::clamp(static_cast<float>(extent.height) / 1000.0f, 0.85f, 2.0f);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    ImGui::NewFrame();
    if (closePopups) ImGui::ClosePopupsExceptModals();
    buildUi();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    // texture uploads submit to the queue and wait for it: under the queue lock (the present thread presents meanwhile)
    if (dd->Textures != nullptr) {
        bool pending = false;
        for (ImTextureData* t : *dd->Textures) pending |= t->Status != ImTextureStatus_OK;
        if (pending) {
            auto lock = device_->ctx().queueLock();
            for (ImTextureData* t : *dd->Textures) {
                if (t->Status != ImTextureStatus_OK) ImGui_ImplVulkan_UpdateTexture(t);
            }
        }
        dd->Textures = nullptr;
    }
    ImGui_ImplVulkan_RenderDrawData(dd, cmd);
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
            ImGui::Text("%.0f fps", n > 0 && sum > 0.0f ? 1000.0f * static_cast<float>(n) / sum : 0.0f);
            ImGui::PopFont();
        }
        ImGui::End();
    }
}

void Overlay::buildSettingsWindow() {
    auto& reg = settings::registry();
    const auto& r = settings::renderer();
    const auto keyLabel = [](const settings::Setting* s) -> std::string {
        if (s == nullptr) return "?";
        std::string l(s->asEnum());
        for (auto& c : l) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return l;
    };
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0f, 700.0f), ImGuiCond_FirstUseEver);
    bool keepOpen = true;
    if (!ImGui::Begin("Risen Enhanced Graphics###rvk_settings", &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!keepOpen) setOpen(false);
        return;
    }

    // ---- A/B ----
    const bool b = reg.bActive();
    ImGui::SeparatorText("A/B comparison");
    ImGui::TextUnformatted("Active side:");
    ImGui::SameLine();
    ImGui::TextColored(b ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%s", sideName(b));
    ImGui::SameLine();
    if (ImGui::Button(std::format("Flip to {} ({})", sideName(!b), keyLabel(r.abKey)).c_str())) reg.flipAB();
    ImGui::SameLine();
    if (ImGui::Button(std::format("Copy {} to {}", sideName(b), sideName(!b)).c_str())) reg.copyActiveToOther();
    ImGui::SameLine();
    if (ImGui::Button(std::format("Reset {} to defaults", sideName(!b)).c_str())) reg.resetOther();
    const auto diffs = reg.abDifferences();
    if (diffs.empty()) {
        ImGui::TextDisabled("Both sides are the same. Side B starts as the defaults (the plain renderer):\n"
                            "change settings on one side, then flip to compare.");
    } else if (ImGui::BeginTable("##ab", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("setting");
        ImGui::TableSetupColumn("A");
        ImGui::TableSetupColumn("B");
        ImGui::TableHeadersRow();
        for (const settings::Setting* s : diffs) {
            const double active = s->value(), other = reg.otherValue(*s);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s->spec().label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(settings::Registry::formatValue(s->spec(), b ? other : active).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(settings::Registry::formatValue(s->spec(), b ? active : other).c_str());
        }
        ImGui::EndTable();
    }

    // ---- settings by category ----
    const auto all = reg.all();
    std::vector<std::string> categories;
    for (const settings::Setting* s : all) {
        if ((s->spec().flags & settings::Flag::Hidden) == 0 && std::ranges::find(categories, s->spec().category) == categories.end()) {
            categories.push_back(s->spec().category);
        }
    }
    for (const std::string& cat : categories) {
        if (!ImGui::CollapsingHeader(cat.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (settings::Setting* s : all) {
            const settings::Spec& sp = s->spec();
            if (sp.category != cat || (sp.flags & settings::Flag::Hidden) != 0) continue;
            ImGui::PushID(sp.key.c_str());
            const bool locked = s->locked();
            // capability resolution (Spec::requirements): greyed, showing the value that applies, with the reason
            const bool unavailable = !s->available();
            const bool disabled = locked || unavailable;
            if (disabled) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            switch (sp.type) {
            case settings::Type::Bool: {
                bool v = s->asBool();
                if (ImGui::Checkbox(sp.label.c_str(), &v)) reg.set(*s, v ? 1.0 : 0.0);
                break;
            }
            case settings::Type::Int: {
                int v = s->asInt();
                if (ImGui::SliderInt(sp.label.c_str(), &v, static_cast<int>(sp.min), static_cast<int>(sp.max),
                                     sp.format.empty() ? "%d" : sp.format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                    reg.set(*s, v);
                }
                break;
            }
            case settings::Type::Float: {
                float v = s->asFloat();
                if (ImGui::SliderFloat(sp.label.c_str(), &v, static_cast<float>(sp.min), static_cast<float>(sp.max),
                                       sp.format.empty() ? "%.2f" : sp.format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                    reg.set(*s, v);
                }
                break;
            }
            case settings::Type::Enum: {
                const auto label = [&](std::size_t i) {
                    return i < sp.optionLabels.size() ? sp.optionLabels[i].c_str() : sp.options[i].c_str();
                };
                const auto cur = static_cast<std::size_t>(s->effective());
                if (ImGui::BeginCombo(sp.label.c_str(), label(cur))) {
                    for (std::size_t i = 0; i < sp.options.size(); ++i) {
                        if (ImGui::Selectable(label(i), i == cur)) reg.set(*s, static_cast<double>(i));
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            }
            if (disabled) ImGui::EndDisabled();
            const std::string unavailableReason = unavailable ? reg.unavailableReason(*s) : std::string();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (unavailable) {
                    ImGui::SetTooltip("%s\n\nUnavailable: %s\nYour choice (%s) is kept and applies once this is available.\n\n"
                                      "%s, default %s",
                                      sp.description.c_str(), unavailableReason.c_str(), reg.text(*s).c_str(), sp.key.c_str(),
                                      settings::Registry::formatValue(sp, sp.defaultValue).c_str());
                } else {
                    ImGui::SetTooltip("%s\n\n%s, default %s", sp.description.c_str(), sp.key.c_str(),
                                      settings::Registry::formatValue(sp, sp.defaultValue).c_str());
                }
            }
            if (unavailable) {
                ImGui::SameLine();
                ImGui::TextDisabled("(unavailable)");
            } else if (locked) {
                ImGui::SameLine();
                ImGui::TextDisabled("(set by config.toml)");
            } else if (!s->isDefault()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("default")) reg.resetToDefault(*s);
            }
            if (s->restartPending()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "restart needed");
            } else if (s->restart()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(after a restart)");
            }
            ImGui::PopID();
        }
    }

    // ---- capabilities (core/caps.h, docs/research/raytracing.md 3) ----
    if (ImGui::CollapsingHeader("Capabilities")) {
        const caps::Report report = reg.capabilities();
        if (!report.known) {
            ImGui::TextDisabled("no Vulkan device");
        } else {
            ImGui::Text("GPU: %s", report.gpu.c_str());
            ImGui::Text("Driver: %s", report.driver.c_str());
            ImGui::Text("Vulkan: %s", report.api.c_str());
            if (ImGui::BeginTable("##caps", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("feature", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < caps::kCount; ++i) {
                    const auto c = static_cast<caps::Cap>(i);
                    const caps::Entry& e = report[c];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::string(caps::label(c)).c_str());
                    if (!e.detail.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", e.detail.c_str());
                    ImGui::TableNextColumn();
                    if (e.enabled) {
                        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "enabled");
                    } else {
                        ImGui::TextColored(e.available ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                                           e.available ? "available, off" : "unavailable");
                        if (!e.reason.empty()) {
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextDisabled("%s", e.reason.c_str());
                            ImGui::PopTextWrapPos();
                        }
                    }
                }
                ImGui::EndTable();
            }
            for (const auto& [k, v] : report.properties) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s: %s", k.c_str(), v.c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }

    // ---- performance ----
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::size_t n = historyCount_;
        float sum = 0.0f, worst = 0.0f, cpuSum = 0.0f, gpuSum = 0.0f;
        std::size_t gpuN = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t k = (historyPos_ + kHistory - 1 - i) % kHistory;
            sum += intervals_[k];
            worst = std::max(worst, intervals_[k]);
            cpuSum += cpu_[k];
            if (gpu_[k] >= 0.0f) {
                gpuSum += gpu_[k];
                ++gpuN;
            }
        }
        const float avg = n > 0 ? sum / static_cast<float>(n) : 0.0f;
        ImGui::Text("%.1f fps   frame %.2f ms (worst %.2f ms over %zu frames)", avg > 0.0f ? 1000.0f / avg : 0.0f, avg, worst, n);
        ImGui::Text("CPU (engine + renderer) %.2f ms", n > 0 ? cpuSum / static_cast<float>(n) : 0.0f);
        if (gpuN > 0) {
            ImGui::Text("GPU %.2f ms", gpuSum / static_cast<float>(gpuN));
        } else {
            ImGui::TextDisabled("GPU time: off (config.toml [debug] perf = true)");
        }
        // the history as a line, oldest first
        float ordered[kHistory];
        for (std::size_t i = 0; i < n; ++i) ordered[i] = intervals_[(historyPos_ + kHistory - n + i) % kHistory];
        ImGui::PlotLines("##frametimes", ordered, static_cast<int>(n), 0, "frame time (ms)", 0.0f, std::max(20.0f, worst * 1.1f),
                         ImVec2(ImGui::GetContentRegionAvail().x, 70.0f));
        const auto& st = device_->stats();
        ImGui::Text("%u draws, vsync %s, frame rate limit %s", st.lastFrameDraws, device_->vsync() ? "on" : "off",
                    device_->maxFps() > 0.0 ? std::format("{:.0f} fps", device_->maxFps()).c_str() : "none");
        // window / display mode (docs/research/window-mode.md 8)
        ImGui::TextUnformatted(engine::window::displaySummary().c_str());
        ImGui::Text("present mode %d%s, frame pacing %.1f fps", static_cast<int>(device_->presentMode()),
                    device_->fullScreenExclusiveHeld()       ? ", Vulkan exclusive fullscreen held"
                    : device_->fullScreenExclusiveConfigured() ? ", exclusive swapchain (not held)"
                                                               : "",
                    device_->pacerRate());
        // enhancements' one-line status (e.g. the RT scene: instances, BLAS, memory)
        for (const auto& [key, text] : device_->status()) ImGui::TextUnformatted(text.c_str());
    }

    // ---- the last frame's phases and camera (d3d9/scene.h, docs/research/frame-phases.md) ----
    if (ImGui::CollapsingHeader("Frame structure")) {
        const auto& ph = device_->phases();
        const std::string seq = d3d9::scene::PhaseTracker::summary(ph.lastFrame());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(seq.empty() ? "(no phase markers)" : seq.c_str());
        ImGui::PopTextWrapPos();
        const auto& dr = device_->drawRecords();
        const std::string kinds = dr.lastHistogram().summary();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("draw kinds (%zu records): %s", dr.last().size(), kinds.c_str());
        ImGui::PopTextWrapPos();
        const auto& c = device_->lastCamera();
        if (c.valid) {
            ImGui::Text("camera (%.1f, %.1f, %.1f), near %.1f, far %.0f", c.position[0], c.position[1], c.position[2], c.nearZ,
                        c.engineFar);
        } else {
            ImGui::TextDisabled("no main view in the last frame");
        }
        // injected passes (d3d9/inject.h, docs/research/injection.md)
        const auto& passes = device_->passes();
        if (passes.empty()) {
            ImGui::TextDisabled("injected passes: none");
        } else {
            std::string list;
            for (std::size_t i = 0; i < d3d9::inject::kPointCount; ++i) {
                const auto point = static_cast<d3d9::inject::Point>(i);
                for (const auto& e : passes.passes(point)) {
                    list += std::format("{}{} at {}", list.empty() ? "" : ", ", e.name, d3d9::inject::name(point));
                }
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("injected passes: %s", list.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s  |  %s: close  |  %s: A/B", reg.dirty() ? "saving..." : "saved in bin\\rvk\\settings.toml",
                        keyLabel(r.toggleKey).c_str(), keyLabel(r.abKey).c_str());
    ImGui::End();
    if (!keepOpen) setOpen(false);
}

}  // namespace rvk::overlay
