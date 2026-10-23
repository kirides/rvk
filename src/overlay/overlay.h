#pragma once

// The in-game overlay (docs/research/overlay.md): Dear ImGui on the Vulkan swapchain with the player settings
// (core/settings.h), the A/B comparison and a performance panel. Toggled with a hotkey (overlay.toggle_key, F11); while it is
// open it takes the game window's keyboard and mouse input (the engine reads WM_KEY*/WM_*BUTTON* messages and WM_INPUT raw
// mouse motion through its window procedure: a subclass of that procedure filters them); while it is closed the game's input
// is untouched apart from the two hotkeys.
//
// Threads: the window procedure runs on the thread that pumps the game window's messages; it only touches atomics, the
// settings registry and a message queue. Everything ImGui runs on the render thread (record()).

#include <atomic>
#include <bitset>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "d3d9/device.h"
#include "overlay/keys.h"

struct HWND__;

namespace rvk::overlay {

struct Options {
    bool intoBackBuffer = false;        // [debug] overlay_in_screenshots
    std::vector<std::string> commands;  // [debug] commands: "<frame>: <command>"
    std::filesystem::path settingsFile; // bin\rvk\settings.toml (the overlay shows its state)
};

class Overlay final : public d3d9::PresentOverlay {
public:
    Overlay() = default;
    ~Overlay() override;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // ImGui + the window subclass; false (logged) when the overlay cannot run
    bool init(d3d9::Device& device, HWND__* window, const Options& options);
    void shutdown();

    bool isOpen() const { return open_.load(std::memory_order_relaxed); }
    void setOpen(bool open);

    // render thread, before each present (frame = the present's number, 1-based): runs the [debug] commands that are due
    void onPresent(std::uint64_t frame);
    // "overlay open|close|toggle", "set <key> <value>", "ab flip|a|b|copy|reset", "window ...", "key <chord> [frames]",
    // "keydown <chord>", "keyup <chord>", "type <text>" (overlay/keys.h), "move <x> <y>", "click <x> <y>": the result / error
    // text in `out`
    bool runCommand(std::string_view command, std::string* out);

    // d3d9::PresentOverlay
    bool active() override;
    void record(VkCommandBuffer cmd, VkExtent2D extent) override;

    // the process's overlay (rvk_overlay), null when none
    static Overlay* instance();

private:
    struct Msg {
        unsigned msg;
        std::uintptr_t wparam;
        std::intptr_t lparam;
    };
    static std::intptr_t __stdcall wndProc(HWND__* hwnd, unsigned msg, std::uintptr_t wparam, std::intptr_t lparam);
    std::intptr_t handleMessage(HWND__* hwnd, unsigned msg, std::uintptr_t wparam, std::intptr_t lparam);
    std::intptr_t callGame(HWND__* hwnd, unsigned msg, std::uintptr_t wparam, std::intptr_t lparam);
    void buildUi();
    void buildSettingsWindow();
    void buildBadges();

    d3d9::Device* device_ = nullptr;
    HWND__* window_ = nullptr;
    Options options_;
    bool initialised_ = false;
    bool imguiVulkan_ = false;
    bool unicode_ = true;
    void* originalProc_ = nullptr;

    std::atomic<bool> open_{false};
    std::atomic<bool> openChanged_{false};  // the render thread resets ImGui's input state
    std::atomic<bool> closePopups_{false};
    std::mutex msgMutex_;
    std::vector<Msg> messages_;             // input for ImGui while open (replayed on the render thread)
    std::bitset<256> gameKeysDown_;         // keys / buttons the game saw pressed (their releases always reach it)
    std::atomic<std::uint32_t> blocked_{0}; // input messages kept from the game (statistics)
    std::atomic<std::uint32_t> wndProcThread_{0};
    bool threadsLogged_ = false;

    // A/B indicator
    bool lastB_ = false;
    std::chrono::steady_clock::time_point flipShownUntil_{};

    // performance panel
    static constexpr std::size_t kHistory = 240;
    float intervals_[kHistory]{};
    float cpu_[kHistory]{};
    float gpu_[kHistory]{};
    std::size_t historyPos_ = 0;
    std::size_t historyCount_ = 0;
    std::uint64_t frame_ = 0;

    struct Command {
        std::uint64_t frame;
        std::string text;
    };
    std::vector<Command> commands_;
    keys::Poster keyPoster_;  // key / keydown / keyup / type
};

}  // namespace rvk::overlay
