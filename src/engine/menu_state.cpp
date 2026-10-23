#include "engine/menu_state.h"

#include <windows.h>

#include <chrono>

#include <spdlog/spdlog.h>

#include "core/exports.h"
#include "engine/symbols.h"

namespace rvk::engine::menu {
namespace {

// Script.dll (docs/research/menu-fps.md 2): bool GUI2::IsLoading() const (0x2105a0), gEHudMode GUI2::GetMode() const
// (0x210520: 0 = the game view shown, 1 = hidden, -1 = no GUI yet), bool GUI2::IsVisible(gEHudPage) const (0x210740). The
// forwarders ignore `this` (they ask gCHudModule2::GetSystem); the exported `gui2` object is passed as risen-spass does.
using IsLoadingFn = bool (*)(const void* gui);
using GetModeFn = int (*)(const void* gui);
using IsVisibleFn = bool (*)(const void* gui, int page);
using AppInstanceFn = void* (*)();
using AppStateFn = unsigned long (*)(const void* app);

struct Gui {
    const void* object = nullptr;
    IsLoadingFn isLoading = nullptr;
    GetModeFn getMode = nullptr;
    IsVisibleFn isVisible = nullptr;
};

Gui g_gui;
bool g_resolved = false;
std::chrono::steady_clock::time_point g_nextResolve{};

// Script.dll is loaded after our DllMain: resolved at the first query that finds it (retried once a second)
bool resolve() {
    if (g_resolved) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return false;
    g_nextResolve = now + std::chrono::seconds(1);
    Gui g;
    g.object = exports::resolve(L"Script.dll", "gui2");
    g.isLoading = reinterpret_cast<IsLoadingFn>(exports::resolve(L"Script.dll", "?IsLoading@GUI2@@QEBA_NXZ"));
    g.getMode = reinterpret_cast<GetModeFn>(exports::resolve(L"Script.dll", "?GetMode@GUI2@@QEBA?AW4gEHudMode@@XZ"));
    g.isVisible = reinterpret_cast<IsVisibleFn>(exports::resolve(L"Script.dll", "?IsVisible@GUI2@@QEBA_NW4gEHudPage@@@Z"));
    if (g.object == nullptr || g.isLoading == nullptr || g.getMode == nullptr || g.isVisible == nullptr) {
        if (exports::moduleBase(L"Script.dll") != 0) {
            spdlog::warn("engine: menu state: Script.dll lacks the GUI2 page exports (object {}, IsLoading {}, GetMode {}, IsVisible {})",
                         g.object != nullptr, g.isLoading != nullptr, g.getMode != nullptr, g.isVisible != nullptr);
            g_nextResolve = (std::chrono::steady_clock::time_point::max)();  // never again
        }
        return false;
    }
    g_gui = g;
    g_resolved = true;
    spdlog::info("engine: menu state: GUI2 page predicates bound");
    return true;
}

constexpr int kPanels[] = {2, 3, 4, 5};  // Inventory, Character, Log, Map

// SEH only (no objects with destructors): fills `out`, false when an engine call faulted
bool queryRaw(const Gui& g, Status& out) {
    const auto& s = sym();
    __try {
        if (s.appGetInstance != nullptr && s.appGetAppState != nullptr) {
            if (const void* app = reinterpret_cast<AppInstanceFn>(s.appGetInstance)(); app != nullptr) {
                out.appState = reinterpret_cast<AppStateFn>(s.appGetAppState)(app);
            }
        }
        if (g.isLoading(g.object)) out.pages |= 1u << 29;
        if ((out.pages & (1u << 29)) != 0 || out.appState == 2 || out.appState == 0x10) {
            out.state = State::Loading;
            return true;
        }
        const int mode = g.getMode(g.object);
        if (mode != 0 && mode != 1) return true;  // no GUI yet: Unknown
        if (mode == 0) out.pages |= 1u << 1;
        bool panel = false;
        for (const int p : kPanels) {
            if (g.isVisible(g.object, p)) {
                out.pages |= 1u << p;
                panel = true;
            }
        }
        out.state = mode == 1 || panel ? State::Menu : State::Game;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

Status query() {
    Status st;
    if (!resolve()) return st;
    if (!queryRaw(g_gui, st)) {
        st = Status{};
        st.faulted = true;
    }
    return st;
}

const char* name(State s) {
    switch (s) {
        case State::Game: return "game";
        case State::Menu: return "menu";
        case State::Loading: return "loading";
        default: return "unknown";
    }
}

}  // namespace rvk::engine::menu
