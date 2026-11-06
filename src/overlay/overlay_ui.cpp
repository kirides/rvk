// The settings window of the overlay (docs/research/overlay.md 2.8): a modern options screen. Header (title, search, advanced
// toggle), the master "Graphics Quality" control, a sidebar of pages and the page's sections of rows (label column + control,
// two columns when the window is wide), a footer with the status, fps / VRAM and "Restore Defaults" / "Apply". The pages are
// generated from the settings' metadata (Spec::page / section / order, tierOf, description = tooltip): nothing here names a
// setting except the presets (core/settings.cpp graphicsPresets).

#include "overlay/overlay.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <thread>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "core/settings.h"
#include "engine/window_mode.h"
#include "rhi/game_device.h"

namespace rvk::overlay {
namespace {

// ---- theme: warm dark, gold accent ----
constexpr ImVec4 kGold(0.86f, 0.69f, 0.31f, 1.0f);
constexpr ImVec4 kGoldDim(0.62f, 0.49f, 0.20f, 1.0f);
constexpr ImVec4 kText(0.91f, 0.87f, 0.79f, 1.0f);
constexpr ImVec4 kTextDim(0.60f, 0.56f, 0.49f, 1.0f);
constexpr ImVec4 kAmber(1.0f, 0.74f, 0.25f, 1.0f);
constexpr ImVec4 kRed(0.90f, 0.35f, 0.28f, 1.0f);
constexpr ImVec4 kGreen(0.55f, 0.80f, 0.40f, 1.0f);
constexpr ImVec4 kPanel(0.062f, 0.053f, 0.046f, 1.0f);
constexpr ImVec4 kFrame(0.15f, 0.13f, 0.105f, 1.0f);

ImU32 col(ImVec4 c, float alpha = 1.0f) {
    c.w *= alpha;
    return ImGui::GetColorU32(c);
}

ImGuiStyle g_base;           // the unscaled theme
ImFont* g_serif = nullptr;   // headings (Georgia Bold)
ImFont* g_semi = nullptr;    // section headings (Segoe UI Semibold)

struct UiState {
    std::string page = settings::kPageDisplay;
    char search[96]{};
    std::string selected;    // the row last clicked / hovered (highlight)
    std::string hover;
    std::chrono::steady_clock::time_point hoverSince{};
    std::string toast;       // "Also switched on: X"
    std::chrono::steady_clock::time_point toastUntil{};
    bool d3d12Checked = false;   // vulkan.api: whether Direct3D 12 can run here (checked once, on first use)
    bool d3d12Ok = true;
    std::string d3d12Reason;
    bool adaptersChecked = false;   // vulkan.adapter: the adapters' names (listed once, on first use)
    std::vector<std::string> adapters;
};
UiState g_ui;

constexpr const char* kPagePerformance = "Performance";

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string alnum(std::string_view s) {
    std::string r;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return r;
}

// `s` cut with "..." to fit `maxW` pixels
std::string fit(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string r = s;
    while (!r.empty() && ImGui::CalcTextSize((r + "...").c_str()).x > maxW) r.pop_back();
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r + "...";
}

const char* pageBlurb(std::string_view page) {
    if (page == settings::kPageDisplay) return "How the game is shown: render mode, frame rate and window behaviour.";
    if (page == settings::kPageLighting) return "Sun shadows, ambient occlusion and the style of light.";
    if (page == settings::kPageRayTracing) return "Hardware ray tracing: shadows, lighting and the quality limits. Needs a capable graphics card.";
    if (page == settings::kPageWater) return "The sea, lakes and rivers, and the look of surfaces.";
    if (page == settings::kPageImage) return "Anti-aliasing, upscaling and frame generation.";
    if (page == settings::kPageDetail) return "How far trees and objects keep their full detail.";
    if (page == kPagePerformance) return "Frame rate, frame times and video memory.";
    if (page == settings::kPageSystem) return "Menu options, graphics hardware and compatibility.";
    if (page == settings::kPageDebug) return "Developer tools: A/B comparison, debug views, internals.";
    return "";
}

// ---- which settings the player sees ----
struct View {
    bool debug = false;
    bool advanced = false;
    bool visible(const settings::Spec& sp) const {
        if (sp.key == "vulkan.api" && !debug) return false;  // the Direct3D 12 backend is not released yet
        switch (settings::tierOf(sp)) {
        case settings::Tier::Hidden: return false;
        case settings::Tier::Debug: return debug;
        case settings::Tier::Advanced: return advanced;
        case settings::Tier::Basic: return true;
        }
        return false;
    }
};

// a value as the overlay shows it: option labels, On / Off, the spec's format, its auto label at the minimum
std::string displayValue(const settings::Spec& sp, double v) {
    switch (sp.type) {
    case settings::Type::Bool: return v != 0.0 ? "On" : "Off";
    case settings::Type::Enum: {
        const auto i = static_cast<std::size_t>(v);
        if (i < sp.optionLabels.size()) return sp.optionLabels[i];
        return i < sp.options.size() ? sp.options[i] : std::string();
    }
    case settings::Type::Int:
    case settings::Type::Float:
        if (!sp.autoLabel.empty() && v == sp.min) return sp.autoLabel;
        if (sp.format.empty()) return settings::Registry::formatValue(sp, v);
        char buf[64];
        if (sp.type == settings::Type::Int) std::snprintf(buf, sizeof(buf), sp.format.c_str(), static_cast<int>(v));
        else std::snprintf(buf, sizeof(buf), sp.format.c_str(), v);
        return buf;
    }
    return {};
}

// a tooltip wrapped at a readable width (descriptions are long single lines)
void wrappedTooltip(const std::string& text, const ImVec2* anchor = nullptr, ImVec2 pivot = ImVec2(0, 0)) {
    if (anchor != nullptr) ImGui::SetNextWindowPos(*anchor, ImGuiCond_Always, pivot);
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// a toggle switch; true when clicked
bool toggleSwitch(const char* id, bool value, bool showText = true) {
    const float em = ImGui::GetFontSize();
    const float tw = em * 2.3f, th = em * 1.2f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float dy = std::max(0.0f, (ImGui::GetFrameHeight() - th) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + dy));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(tw, th));
    const bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImU32 track = value ? col(kGoldDim, hover ? 1.0f : 0.9f) : col(ImVec4(0.26f, 0.23f, 0.19f, 1.0f), hover ? 1.0f : 0.9f);
    dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + th), track, th * 0.5f);
    dl->AddRect(a, ImVec2(a.x + tw, a.y + th), col(kGold, value ? 0.7f : 0.25f), th * 0.5f);
    const float kr = th * 0.5f - 2.5f;
    const float kx = value ? a.x + tw - th * 0.5f : a.x + th * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, a.y + th * 0.5f), kr, col(value ? ImVec4(1.0f, 0.93f, 0.75f, 1.0f) : ImVec4(0.62f, 0.58f, 0.52f, 1.0f)));
    if (showText) {
        const char* t = value ? "On" : "Off";
        dl->AddText(ImVec2(a.x + tw + em * 0.5f, p.y + (ImGui::GetFrameHeight() - em) * 0.5f), col(value ? kText : kTextDim), t);
    }
    return clicked;
}

// ---- dependencies (the registry's Spec::needs) ----
// The group parent of a dependent: its first prerequisite that is switched on for the player (a render-mode need only greys it).
settings::Setting* parentOf(settings::Registry& reg, const settings::Setting& s) {
    for (const settings::Need& n : s.spec().needs) {
        if (n.autoApply) return reg.find(n.key);
    }
    return nullptr;
}

// why `s` is greyed by its prerequisites ("" = none)
std::string needsOffDepth(settings::Registry& reg, const settings::Setting& s, int depth) {
    std::string r = reg.needsReason(s);
    if (!r.empty() || depth > 5) return r;
    // a prerequisite that is itself unavailable (capability / config lock) greys its dependents the same way
    for (const settings::Need& n : s.spec().needs) {
        const settings::Setting* p = reg.find(n.key);
        if (p == nullptr) continue;
        if (!p->available()) return p->spec().label + " is unavailable";
        r = needsOffDepth(reg, *p, depth + 1);
        if (!r.empty()) return r;
    }
    return r;
}
std::string needsOff(settings::Registry& reg, const settings::Setting& s) { return needsOffDepth(reg, s, 0); }

// a change made by the player: the unmet prerequisites are switched on first (the registry names them: a toast; restart-only ones
// show up in the restart banner like any restart setting)
void userSet(settings::Registry& reg, settings::Setting& s, double value) {
    if (s.type() != settings::Type::Bool && s.type() != settings::Type::Enum) {
        reg.set(s, value);
        return;
    }
    const settings::Registry::NeedsResult r = reg.setWithNeeds(s.key(), settings::Registry::formatValue(s.spec(), value));
    if (r.changes.empty()) return;
    std::string names;
    for (const settings::Registry::NeedChange& c : r.changes) {
        const settings::Setting* p = reg.find(c.key);
        if (p == nullptr) continue;
        if (!names.empty()) names += ", ";
        names += p->spec().label;
    }
    g_ui.toast = "Also switched on: " + names;
    g_ui.toastUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
}

// ---- one setting row ----
struct RowGeometry {
    float height;
    bool note;
};

RowGeometry rowGeometry(const settings::Setting& s, bool depNote) {
    const float em = ImGui::GetFontSize();
    const bool note = !s.available() || s.locked() || s.spec().key == "vulkan.api" || (depNote && !needsOff(settings::registry(), s).empty());
    return {ImGui::GetFrameHeight() + em * 0.55f + (note ? em * 0.95f : 0.0f), note};
}

// draws the row at screen position `p`, `w` wide; the caller advances by the row's height
void drawRow(settings::Registry& reg, settings::Setting& s, ImVec2 p, float w, bool debug, int indent, bool depNote) {
    const float emI = ImGui::GetFontSize();
    // a dependent: indented under its prerequisite, dimmed while that is off
    const std::string needReason = needsOff(reg, s);
    const bool parentOff = !needReason.empty();
    if (indent > 0) {
        const float d = emI * 1.3f * static_cast<float>(indent);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + emI * 0.55f, p.y), ImVec2(p.x + emI * 0.55f, p.y + ImGui::GetFrameHeight() * 0.85f), col(kGold, 0.30f));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + emI * 0.55f, p.y + ImGui::GetFrameHeight() * 0.85f), ImVec2(p.x + emI * 1.15f, p.y + ImGui::GetFrameHeight() * 0.85f), col(kGold, 0.30f));
        p.x += d;
        w -= d;
    }
    const settings::Spec& sp = s.spec();
    const float em = ImGui::GetFontSize();
    const RowGeometry g = rowGeometry(s, depNote);
    const float frameH = ImGui::GetFrameHeight();
    const float rowH = frameH + em * 0.55f;
    const ImVec2 rmin = p, rmax(p.x + w, p.y + g.height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(sp.key.c_str());

    const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    const bool hovered = windowHovered && ImGui::IsMouseHoveringRect(rmin, rmax, false);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_ui.selected = sp.key;
    const bool selected = g_ui.selected == sp.key;
    dl->AddLine(ImVec2(rmin.x + em * 0.5f, rmax.y - 1.0f), ImVec2(rmax.x - em * 0.5f, rmax.y - 1.0f), col(ImVec4(1, 1, 1, 1), 0.04f));  // row separator
    if (selected) {
        dl->AddRectFilled(rmin, rmax, col(kGold, 0.10f), em * 0.25f);
        dl->AddRectFilled(rmin, ImVec2(rmin.x + 3.0f, rmax.y), col(kGold, 0.9f));
    } else if (hovered) {
        dl->AddRectFilled(rmin, rmax, col(ImVec4(1, 1, 1, 1), 0.045f), em * 0.25f);
    }
    if (hovered) {
        if (g_ui.hover != sp.key) {
            g_ui.hover = sp.key;
            g_ui.hoverSince = std::chrono::steady_clock::now();
        }
    }

    const bool locked = s.locked();
    const bool unavailable = !s.available();
    const bool disabled = locked || unavailable;
    if (parentOff && !disabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    const float gutter = em * 1.25f;
    const float labelW = (w - gutter) * 0.45f;
    const float resetW = em * 3.3f;
    const float xc = p.x + gutter + labelW;                       // control column
    const float cw = std::clamp(w - (xc - p.x) - resetW - em * 0.3f, em * 4.0f, em * 20.0f);
    const float ty = p.y + (rowH - em) * 0.5f;                    // text baseline row

    // changed-from-default dot
    if (!s.isDefault()) dl->AddCircleFilled(ImVec2(p.x + em * 0.6f, p.y + rowH * 0.5f), em * 0.17f, col(kGold));

    // label (+ restart badge)
    std::string badge;
    ImVec4 badgeCol = kTextDim;
    if (s.restartPending()) {
        badge = "restart needed";
        badgeCol = kAmber;
    } else if (s.restart()) {
        badge = "restart";
    }
    float badgeW = 0.0f;
    if (!badge.empty()) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        badgeW = ImGui::CalcTextSize(badge.c_str()).x + em * 0.7f;
        ImGui::PopFont();
    }
    const std::string label = fit(sp.label, labelW - badgeW - em * 0.9f);
    dl->AddText(ImVec2(p.x + gutter, ty), col(disabled ? kTextDim : kText), label.c_str());
    if (!badge.empty()) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        const float bx = p.x + gutter + labelW - badgeW - em * 0.6f;
        const float bh = ImGui::GetFontSize() + em * 0.2f;
        const float by = p.y + (rowH - bh) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + badgeW - em * 0.15f, by + bh), col(badgeCol, 0.16f), bh * 0.5f);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + badgeW - em * 0.15f, by + bh), col(badgeCol, 0.7f), bh * 0.5f);
        dl->AddText(ImVec2(bx + em * 0.3f, by + em * 0.1f), col(badgeCol), badge.c_str());
        ImGui::PopFont();
    }

    // the control
    ImGui::SetCursorScreenPos(ImVec2(xc, p.y + (rowH - frameH) * 0.5f));
    if (disabled) ImGui::BeginDisabled();
    switch (sp.type) {
    case settings::Type::Bool: {
        if (toggleSwitch("##v", s.asBool())) userSet(reg, s, s.asBool() ? 0.0 : 1.0);
        break;
    }
    case settings::Type::Int:
    case settings::Type::Float: {
        if (sp.key == "vulkan.adapter") {
            if (!g_ui.adaptersChecked) {
                g_ui.adaptersChecked = true;
                for (const rhi::AdapterInfo& a : rhi::lastEnumeratedAdapters()) g_ui.adapters.push_back(a.name);
            }
            const int cur = s.asInt();
            const std::string curText = cur < 0 ? "Auto (the fastest card)"
                                        : static_cast<std::size_t>(cur) < g_ui.adapters.size() ? g_ui.adapters[static_cast<std::size_t>(cur)]
                                                                                              : std::format("Card {}", cur);
            ImGui::SetNextItemWidth(cw);
            if (ImGui::BeginCombo("##v", curText.c_str())) {
                if (ImGui::Selectable("Auto (the fastest card)", cur < 0)) userSet(reg, s, -1.0);
                for (std::size_t i = 0; i < g_ui.adapters.size(); ++i) {
                    if (ImGui::Selectable(std::format("{}: {}", i, g_ui.adapters[i]).c_str(), static_cast<int>(i) == cur)) userSet(reg, s, static_cast<double>(i));
                }
                ImGui::EndCombo();
            }
            break;
        }
        const bool isInt = sp.type == settings::Type::Int;
        float v = static_cast<float>(isInt ? s.asInt() : s.asFloat());
        const float valueW = em * 5.4f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
        ImGui::SetNextItemWidth(cw - valueW);
        bool changed = false;
        if (isInt) {
            int iv = static_cast<int>(v);
            changed = ImGui::SliderInt("##v", &iv, static_cast<int>(sp.min), static_cast<int>(sp.max), "", ImGuiSliderFlags_AlwaysClamp);
            if (changed) userSet(reg, s, iv);
            v = static_cast<float>(iv);
        } else {
            changed = ImGui::SliderFloat("##v", &v, static_cast<float>(sp.min), static_cast<float>(sp.max), "", ImGuiSliderFlags_AlwaysClamp);
            if (changed) userSet(reg, s, v);
        }
        ImGui::PopStyleColor(5);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
        const float frac = sp.max > sp.min ? std::clamp((v - static_cast<float>(sp.min)) / static_cast<float>(sp.max - sp.min), 0.0f, 1.0f) : 0.0f;
        const float x0 = a.x + em * 0.45f, x1 = b.x - em * 0.45f, cy = (a.y + b.y) * 0.5f, th = em * 0.28f;
        const float kx = x0 + (x1 - x0) * frac;
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), col(ImVec4(0.30f, 0.26f, 0.19f, 1.0f)), th);
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(kx, cy + th * 0.5f), col(kGoldDim), th);
        dl->AddCircleFilled(ImVec2(kx, cy), em * (hov ? 0.52f : 0.44f), col(hov ? ImVec4(1.0f, 0.88f, 0.55f, 1.0f) : kGold));
        const std::string vt = fit(displayValue(sp, v), valueW - em * 0.4f);
        dl->AddText(ImVec2(b.x + em * 0.7f, cy - em * 0.5f), col(kText), vt.c_str());
        break;
    }
    case settings::Type::Enum: {
        // Low bandwidth mode's "Auto" says what it resolved to: "Auto (on: integrated GPU)"
        std::string autoLabel;
        if (sp.key == "vulkan.low_bandwidth") {
            const caps::LowBandwidth d = caps::lowBandwidthDecision();
            if (d.automatic) autoLabel = std::format("Auto ({}: {})", d.on ? "on" : "off", d.reason);
        }
        const auto optLabel = [&](std::size_t i) {
            if (i == 0 && !autoLabel.empty()) return autoLabel.c_str();
            return i < sp.optionLabels.size() ? sp.optionLabels[i].c_str() : sp.options[i].c_str();
        };
        const auto cur = static_cast<std::size_t>(s.effective());
        ImGui::SetNextItemWidth(cw);
        if (ImGui::BeginCombo("##v", optLabel(cur))) {
            for (std::size_t i = 0; i < sp.options.size(); ++i) {
                // an option whose capability is missing (Spec::optionRequirements) is shown greyed, with the reason
                const bool optionOk = s.optionAvailable(static_cast<double>(i)) && !(sp.key == "vulkan.api" && i == 1 && !g_ui.d3d12Ok);
                if (!optionOk) ImGui::BeginDisabled();
                if (ImGui::Selectable(optLabel(i), i == cur)) userSet(reg, s, static_cast<double>(i));
                if (!optionOk) ImGui::EndDisabled();
                if (!optionOk && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                    wrappedTooltip("Unavailable: " + (sp.key == "vulkan.api" ? g_ui.d3d12Reason : reg.optionUnavailableReason(s, i)));
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    }
    if (disabled) ImGui::EndDisabled();
    const bool controlHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (controlHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left))) g_ui.selected = sp.key;

    // reset to default
    if (!s.isDefault() && !locked) {
        ImGui::SetCursorScreenPos(ImVec2(xc + cw + em * 0.35f, p.y + (rowH - frameH) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kGold.x, kGold.y, kGold.z, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kGold.x, kGold.y, kGold.z, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        if (ImGui::Button("Reset", ImVec2(resetW, frameH))) reg.resetToDefault(s);
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Back to the default: %s", displayValue(sp, sp.defaultValue).c_str());
    }

    // the reason a row is greyed
    if (g.note) {
        std::string note;
        if (unavailable) {
            // the reason reads "<capability>: <detail>": the detail alone says it in less room (the tooltip has the whole text)
            note = reg.unavailableReason(s);
            const std::size_t colon = note.find(": ");
            if (colon != std::string::npos && colon < 40) note = note.substr(colon + 2);
            if (!note.empty()) note[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(note[0])));
            note = "Unavailable: " + note;
        } else {
            note = "Set by rvk\\config.toml for this session";
        }
        bool amber = unavailable;
        if (!unavailable && !locked && parentOff && depNote) {
            note = "Greyed: " + needReason;
            amber = true;
        } else if (!unavailable && !locked && sp.key == "vulkan.api") {
            note = g_ui.d3d12Ok ? "Restart required. Direct3D 12 is experimental." : "Direct3D 12 is not usable here: " + g_ui.d3d12Reason;
            amber = !g_ui.d3d12Ok;
        }
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
        note = fit(note, w - gutter - em * 0.3f);
        dl->AddText(ImVec2(p.x + gutter, p.y + rowH - em * 0.1f), col(amber ? kAmber : kTextDim, 1.0f), note.c_str());
        ImGui::PopFont();
    }

    // tooltip after a short rest on the row (never while a popup is open)
    if (hovered && g_ui.hover == sp.key && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        std::chrono::steady_clock::now() - g_ui.hoverSince > std::chrono::milliseconds(800) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        std::string tip = sp.description;
        if (unavailable) {
            tip += std::format("\n\nUnavailable: {}\nYour choice ({}) is kept and applies once this is available.", reg.unavailableReason(s),
                               displayValue(sp, s.value()));
        }
        if (locked) tip += "\n\nSet by rvk\\config.toml for this session.";
        if (s.restart()) tip += "\n\nTakes effect after a restart of the game.";
        tip += std::format("\n\nDefault: {}", displayValue(sp, sp.defaultValue));
        if (debug) tip += std::format("  ({} = {})", sp.key, settings::Registry::formatValue(sp, s.value()));
        // beside the row, never over its neighbours: right of the control in the left column, left of the label in the right one
        const ImVec2 mid = ImGui::GetMousePos();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const bool leftHalf = p.x + w * 0.5f < vp->Pos.x + vp->Size.x * 0.5f;
        const ImVec2 at = leftHalf ? ImVec2(xc + cw + resetW + em * 1.0f, rmin.y) : ImVec2(rmin.x - em * 0.5f, rmin.y);
        (void)mid;
        wrappedTooltip(tip, &at, leftHalf ? ImVec2(0, 0) : ImVec2(1, 0));
    }
    if (parentOff && !disabled) ImGui::PopStyleVar();
    ImGui::PopID();
}

// a section heading: small caps in gold, a thin separator under it
void sectionHeading(const std::string& text, float width) {
    const float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(1, em * 0.5f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_semi, ImGui::GetStyle().FontSizeBase * 0.84f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + em * 0.25f, p.y));
    ImGui::TextColored(ImVec4(kGold.x, kGold.y, kGold.z, 0.95f), "%s", upper(text).c_str());
    ImGui::PopFont();
    const float y = ImGui::GetCursorScreenPos().y + em * 0.1f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y), ImVec2(p.x + width, y), col(kGold, 0.28f), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, y + em * 0.35f));
}

// the rows of one section in `columns` columns: a prerequisite and its dependents stay together (dependents indented under it);
// two columns fill top to bottom, the groups split where the first holds about half the height
void drawRows(settings::Registry& reg, const std::vector<settings::Setting*>& items, float width, int columns, bool debug) {
    const float em = ImGui::GetFontSize();
    const float gap = em * 1.8f;
    const float cell = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    struct Row {
        settings::Setting* s;
        int indent;
        bool note;  // the first dependent of a group carries the "greyed because" line
    };
    std::vector<std::vector<Row>> groups;
    {
        // roots: settings whose prerequisite is not in this section; dependents (any depth) are listed under their root in order
        const auto inItems = [&](const settings::Setting* x) { return x != nullptr && std::ranges::find(items, x) != items.end(); };
        const auto rootOf = [&](settings::Setting* x, int& depth) {
            depth = 0;
            while (depth < 4) {
                settings::Setting* par = parentOf(reg, *x);
                if (!inItems(par) || par == x) break;
                x = par;
                ++depth;
            }
            return x;
        };
        for (settings::Setting* x : items) {
            int d = 0;
            if (rootOf(x, d) == x) groups.push_back({{x, 0, false}});
        }
        for (settings::Setting* x : items) {
            int d = 0;
            settings::Setting* root = rootOf(x, d);
            if (root == x) continue;
            for (auto& g : groups) {
                if (g.front().s == root) {
                    g.push_back({x, std::min(d, 2), g.size() == 1});
                    break;
                }
            }
        }
    }
    float total = 0.0f;
    for (const auto& g : groups) {
        for (const Row& r : g) total += rowGeometry(*r.s, r.note).height;
    }
    std::vector<float> y(static_cast<std::size_t>(columns), origin.y);
    float acc = 0.0f;
    int column = 0;
    for (const auto& g : groups) {
        float gh = 0.0f;
        for (const Row& r : g) gh += rowGeometry(*r.s, r.note).height;
        if (columns == 2 && column == 0 && acc > 0.0f && acc + gh * 0.5f > total * 0.5f) column = 1;
        for (const Row& r : g) {
            const float x = origin.x + static_cast<float>(column) * (cell + gap);
            drawRow(reg, *r.s, ImVec2(x, y[static_cast<std::size_t>(column)]), cell, debug, r.indent, r.note);
            y[static_cast<std::size_t>(column)] += rowGeometry(*r.s, r.note).height;
        }
        acc += gh;
    }
    const float bottom = *std::ranges::max_element(y);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
    ImGui::Dummy(ImVec2(1, em * 0.3f));
}

struct Section {
    std::string name;
    int order = 0;
    std::size_t first = 0;
    std::vector<settings::Setting*> items;
};

// the visible settings of a page grouped into sections: by Spec::order then first registration; `filter` narrows them
template <class Pred>
std::vector<Section> collectSections(const settings::Registry& reg, const View& view, Pred&& pred, bool pageInName) {
    std::vector<Section> sections;
    std::size_t idx = 0;
    for (settings::Setting* s : reg.all()) {
        ++idx;
        const settings::Spec& sp = s->spec();
        if (!view.visible(sp) || !pred(sp)) continue;
        const std::string name = pageInName ? settings::pageOf(sp) + "  -  " + settings::sectionOf(sp) : settings::sectionOf(sp);
        auto it = std::ranges::find(sections, name, &Section::name);
        if (it == sections.end()) {
            sections.push_back({name, sp.order, idx, {}});
            it = sections.end() - 1;
        }
        it->order = std::min(it->order, sp.order);
        it->items.push_back(s);
    }
    std::ranges::stable_sort(sections, [](const Section& a, const Section& b) { return a.order != b.order ? a.order < b.order : a.first < b.first; });
    for (Section& sec : sections) {
        std::ranges::stable_sort(sec.items, [](const settings::Setting* a, const settings::Setting* b) { return a->spec().order < b->spec().order; });
    }
    return sections;
}

bool matches(const settings::Spec& sp, const std::vector<std::string>& words) {
    const std::string hay = lower(sp.label + " " + sp.key + " " + sp.category + " " + settings::sectionOf(sp) + " " + settings::pageOf(sp) + " " + sp.description);
    return std::ranges::all_of(words, [&](const std::string& w) { return hay.find(w) != std::string::npos; });
}

std::vector<std::string> searchWords(const char* text) {
    std::vector<std::string> words;
    std::string cur;
    for (const char* c = text; ; ++c) {
        if (*c == 0 || *c == ' ') {
            if (!cur.empty()) words.push_back(lower(cur));
            cur.clear();
            if (*c == 0) break;
        } else {
            cur.push_back(*c);
        }
    }
    return words;
}

// the render mode explained, under its setting (the active mode lit)
void renderModeNotes(const settings::Setting* mode) {
    const float em = ImGui::GetFontSize();
    struct Entry {
        const char* name;
        const char* text;
    };
    static const Entry entries[] = {
        {"Original", "The game's own Direct3D 9 renderer. No Vulkan, no enhancements, no in-game menu (after a restart)."},
        {"Wrapper", "The game's own rendering translated to Vulkan, plus every enhancement. Recommended."},
        {"Full", "The renderer draws the whole frame itself from the game's data. Experimental, costs some processor time."},
    };
    const int active = mode != nullptr ? static_cast<int>(mode->value()) : -1;
    ImGui::Dummy(ImVec2(1, em * 0.2f));
    const float x0 = ImGui::GetCursorScreenPos().x + em * 1.25f;
    for (int i = 0; i < 3; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        ImGui::TextColored(i == active ? kGold : kTextDim, "%s", entries[i].name);
        ImGui::SameLine(x0 - ImGui::GetWindowPos().x + ImGui::GetScrollX() + em * 6.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, i == active ? kText : kTextDim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(entries[i].text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(1, em * 0.3f));
}

// ---- restart ----
void relaunchGame(HWND window, const std::filesystem::path& settingsFile) {
    // settings.toml was saved by the caller. A helper shell waits a few seconds, then starts the game again with the same command line;
    // this process asks the window to close and is ended after 3 s if it has not left by itself.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t dir[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, dir);
    std::wstring cmd = L"cmd.exe /d /c \"ping -n 6 127.0.0.1 >nul & start \"\" /d \"" + std::wstring(dir) + L"\" " + GetCommandLineW() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, dir, &si, &pi)) {
        spdlog::error("overlay: relaunch failed ({})", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    spdlog::info("overlay: relaunching the game ({}), settings in {}", std::filesystem::path(exe).string(), settingsFile.string());
    PostMessageW(window, WM_CLOSE, 0, 0);
    std::thread([] {
        Sleep(3000);
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
}

}  // namespace

// ---- theme ----------------------------------------------------------------------------------------------------------

void Overlay::initTheme() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.0f;
    st.ChildRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.PopupRounding = 6.0f;
    st.GrabRounding = 4.0f;
    st.ScrollbarRounding = 6.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.ChildBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
    st.FramePadding = ImVec2(9.0f, 5.0f);
    st.ItemSpacing = ImVec2(9.0f, 6.0f);
    st.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
    st.ScrollbarSize = 8.0f;
    st.GrabMinSize = 12.0f;
    st.SeparatorTextBorderSize = 1.0f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_WindowBg] = ImVec4(0.092f, 0.079f, 0.067f, 0.985f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.115f, 0.098f, 0.082f, 0.99f);
    c[ImGuiCol_Border] = ImVec4(kGold.x, kGold.y, kGold.z, 0.32f);
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.18f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.22f, 0.16f, 1.0f);
    c[ImGuiCol_TitleBg] = kPanel;
    c[ImGuiCol_TitleBgActive] = kPanel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.50f, 0.40f, 0.20f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = kGoldDim;
    c[ImGuiCol_ScrollbarGrabActive] = kGold;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGold;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.86f, 0.5f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.17f, 0.13f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.27f, 0.17f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.36f, 0.16f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(kGold.x, kGold.y, kGold.z, 0.20f);
    c[ImGuiCol_HeaderHovered] = ImVec4(kGold.x, kGold.y, kGold.z, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(kGold.x, kGold.y, kGold.z, 0.42f);
    c[ImGuiCol_Separator] = ImVec4(kGold.x, kGold.y, kGold.z, 0.25f);
    c[ImGuiCol_TableHeaderBg] = kPanel;
    c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_PlotLines] = kGold;
    c[ImGuiCol_PlotHistogram] = kGoldDim;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kGold.x, kGold.y, kGold.z, 0.35f);
    c[ImGuiCol_NavCursor] = kGold;

    // fonts: Segoe UI body, Georgia Bold headings, Segoe UI Semibold section headings (each optional: the body font stands in)
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const auto load = [&](const wchar_t* name) -> ImFont* {
        const std::filesystem::path path = std::filesystem::path(winDir) / L"Fonts" / name;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return nullptr;
        const std::u8string u8 = path.u8string();
        return io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(u8.c_str()), 18.0f);
    };
    ImFont* body = load(L"segoeui.ttf");
    if (body == nullptr) io.Fonts->AddFontDefault();
    g_semi = load(L"seguisb.ttf");
    g_serif = load(L"georgiab.ttf");
    if (g_serif == nullptr) g_serif = load(L"georgia.ttf");
    st.FontSizeBase = 18.0f;
    g_base = st;
}

void Overlay::applyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_base;
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

// ---- VRAM ----------------------------------------------------------------------------------------------------------

void Overlay::buildVramBar(float width, bool label) {
    const auto now = std::chrono::steady_clock::now();
    if (now - vramAt_ > std::chrono::milliseconds(500)) {
        vramAt_ = now;
        std::uint64_t used = 0, budget = 0;
        if (!device_->game().videoMemory(used, budget)) used = budget = 0;
        vramUsed_ = used;
        vramBudget_ = budget;
    }
    const float em = ImGui::GetFontSize();
    if (vramBudget_ == 0) {
        ImGui::TextDisabled("VRAM: unknown");
        return;
    }
    const float frac = std::clamp(static_cast<float>(static_cast<double>(vramUsed_) / static_cast<double>(vramBudget_)), 0.0f, 1.0f);
    const ImVec4 bar = frac > 0.92f ? kRed : frac > 0.78f ? kAmber : kGold;
    char text[64];
    std::snprintf(text, sizeof(text), "%.1f / %.1f GB", static_cast<double>(vramUsed_) / 1073741824.0, static_cast<double>(vramBudget_) / 1073741824.0);
    if (label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("VRAM");
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.17f, 0.13f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, em * 0.4f);
    ImGui::ProgressBar(frac, ImVec2(width, em * 1.15f), text);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// ---- the window -----------------------------------------------------------------------------------------------------

void Overlay::buildSettingsWindow() {
    auto& reg = settings::registry();
    const auto& r = settings::renderer();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const float em = ImGui::GetFontSize();
    const float baseSize = ImGui::GetStyle().FontSizeBase;
    View view;
    view.debug = reg.debugMode();
    view.advanced = view.debug || (r.showAdvanced != nullptr && r.showAdvanced->value() != 0.0);
    const auto keyLabel = [](const settings::Setting* s) -> std::string { return s != nullptr ? upper(s->asEnum()) : "?"; };

    if (!g_ui.d3d12Checked) {
        g_ui.d3d12Checked = true;
        std::string reason;
        g_ui.d3d12Ok = rhi::d3d12Usable(-1, &reason);
        g_ui.d3d12Reason = g_ui.d3d12Ok ? std::string() : reason;
        spdlog::info("overlay: Direct3D 12 here: {}", g_ui.d3d12Ok ? "usable" : reason);
    }
    // [debug] command "overlay tab <name>": a page by (a prefix of) its name, ignoring case / punctuation; old tab names as aliases
    if (!selectTab_.empty()) {
        const std::string want = alnum(selectTab_);
        std::vector<std::string> pages = settings::pageOrder();
        pages.push_back(kPagePerformance);
        for (const std::string& page : pages) {
            if (!want.empty() && alnum(page).starts_with(want)) {
                g_ui.page = page;
                g_ui.search[0] = 0;
                break;
            }
        }
        if (want == "graphics") g_ui.page = settings::kPageLighting;
        if (want == "perf") g_ui.page = kPagePerformance;
        if (want == "rt") g_ui.page = settings::kPageRayTracing;
        selectTab_.clear();
    }
    if (g_ui.page == settings::kPageDebug && !view.debug) g_ui.page = settings::kPageDisplay;

    // dim backdrop (the game stays visible behind)
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, col(ImVec4(0.02f, 0.015f, 0.01f, 1.0f), 0.62f));

    const float winW = std::min(display.x * 0.95f, em * 92.0f);
    const float winH = std::min(display.y * 0.92f, em * 54.0f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool begun = ImGui::Begin("##rvk_settings", nullptr,
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!begun) {
        ImGui::End();
        return;
    }
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = em * 1.3f;
    const float headerH = em * 3.9f, qualityH = em * 3.3f, footerH = em * 3.3f, sideW = em * 15.0f;
    const float frameH = ImGui::GetFrameHeight();

    // ---- header: title, search, advanced, close ----
    ImGui::SetCursorPos(ImVec2(pad, em * 0.5f));
    ImGui::PushFont(g_serif, baseSize * 2.1f);
    ImGui::TextColored(kGold, "Risen Enhanced Graphics");
    ImGui::PopFont();
    {
        float x = W - pad;
        const float cy = (headerH - frameH) * 0.5f;
        const float closeW = em * 5.0f;
        x -= closeW;
        ImGui::SetCursorPos(ImVec2(x, cy));
        if (ImGui::Button("Close", ImVec2(closeW, frameH))) setOpen(false);
        x -= em * 1.4f;
        const char* advLabel = "Advanced";
        const float advW = em * 2.3f + em * 0.6f + ImGui::CalcTextSize(advLabel).x;
        x -= advW;
        ImGui::SetCursorPos(ImVec2(x, cy));
        if (view.debug) ImGui::BeginDisabled();
        if (toggleSwitch("##adv", view.advanced, false) && r.showAdvanced != nullptr) reg.set(*r.showAdvanced, view.advanced ? 0.0 : 1.0);
        ImGui::SameLine(0.0f, em * 0.6f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(advLabel);
        if (view.debug) ImGui::EndDisabled();
        if (ImGui::IsMouseHoveringRect(ImVec2(wpos.x + x, wpos.y + cy), ImVec2(wpos.x + x + advW, wpos.y + cy + frameH)) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            ImGui::SetTooltip("%s", view.debug ? "Always on in debug mode." : "Also show the fine-tuning settings (wave height, shadow distance, ...).");
        }
        x -= em * 1.4f;
        const float searchW = em * 15.0f;
        x -= searchW;
        ImGui::SetCursorPos(ImVec2(x, cy));
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##search", "Search settings", g_ui.search, sizeof(g_ui.search));
    }
    dl->AddLine(ImVec2(wpos.x, wpos.y + headerH), ImVec2(wpos.x + W, wpos.y + headerH), col(kGold, 0.45f));

    // ---- master quality ----
    {
        const auto& presets = settings::graphicsPresets();
        const int current = settings::matchPreset(reg);
        ImGui::SetCursorPos(ImVec2(pad, headerH + (qualityH - frameH) * 0.5f));
        ImGui::PushFont(g_semi, baseSize * 0.9f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kGold, "GRAPHICS QUALITY");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, em * 1.2f);
        const float segW = em * 6.0f;
        const auto segment = [&](const char* label, bool active, bool clickable) {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, kGold);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.82f, 0.45f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGold);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.03f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.90f, 0.62f, 0.9f));
            }
            const bool clicked = ImGui::Button(label, ImVec2(segW, frameH)) && clickable;
            if (active) ImGui::PopStyleColor(5);
            return clicked;
        };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.25f, 0));
        for (std::size_t i = 0; i < presets.size(); ++i) {
            if (i > 0) ImGui::SameLine();
            if (segment(presets[i].label.c_str(), static_cast<int>(i) == current, true)) {
                settings::applyPreset(reg, presets[i]);
                spdlog::info("overlay: preset {}", presets[i].name);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", presets[i].name == "original" ? "The game's own look: the enhancements off."
                                        : presets[i].name == "high"   ? "The recommended defaults."
                                                                      : "Sets the water, sea, sun shadow and ray tracing quality together.");
            }
        }
        ImGui::SameLine();
        segment("Custom", current < 0, false);
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, em * 1.2f);
        ImGui::AlignTextToFramePadding();
        {
            const char* line = "Your own mix of settings. Pick a preset to start over from it.";
            if (current >= 0) {
                const std::string& nm = presets[static_cast<std::size_t>(current)].name;
                line = nm == "original" ? "The game's own look: every enhancement off."
                       : nm == "low"    ? "Light on the graphics card: the cheapest enhancements."
                       : nm == "medium" ? "A balance of looks and speed."
                       : nm == "high"   ? "The recommended defaults."
                                        : "Everything on, for the strongest graphics cards.";
            }
            ImGui::TextColored(current >= 0 ? kText : kTextDim, "%s", line);
        }
    }
    dl->AddLine(ImVec2(wpos.x, wpos.y + headerH + qualityH), ImVec2(wpos.x + W, wpos.y + headerH + qualityH), col(kGold, 0.25f));

    const bool searching = g_ui.search[0] != 0;
    const float bodyY = headerH + qualityH;
    const float bodyH = H - bodyY - footerH;

    // ---- sidebar ----
    {
        // pages with something to show
        std::vector<std::string> pages;
        for (const std::string& page : settings::pageOrder()) {
            if (page == settings::kPageDebug && !view.debug) continue;
            bool any = false;
            for (const settings::Setting* s : reg.all()) {
                if (view.visible(s->spec()) && settings::pageOf(s->spec()) == page) {
                    any = true;
                    break;
                }
            }
            if (!any) continue;
            if (page == settings::kPageSystem) pages.push_back(kPagePerformance);
            pages.push_back(page);
        }
        ImGui::SetCursorPos(ImVec2(0, bodyY));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("##side", ImVec2(sideW, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1, em * 0.6f));
        const float itemH = em * 2.55f;
        for (const std::string& page : pages) {
            const bool sel = !searching && page == g_ui.page;
            ImGui::PushID(page.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##page", ImVec2(sideW, itemH))) {
                g_ui.page = page;
                g_ui.search[0] = 0;
            }
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            if (sel) {
                sdl->AddRectFilled(p, ImVec2(p.x + sideW, p.y + itemH), col(kGold, 0.13f));
                sdl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + itemH), col(kGold));
            } else if (hov) {
                sdl->AddRectFilled(p, ImVec2(p.x + sideW, p.y + itemH), col(ImVec4(1, 1, 1, 1), 0.05f));
            }
            ImGui::PushFont(sel ? g_semi : nullptr, 0.0f);
            sdl->AddText(ImVec2(p.x + em * 1.3f, p.y + (itemH - em) * 0.5f), col(sel ? kGold : (hov ? kText : ImVec4(0.78f, 0.74f, 0.66f, 1.0f))), page.c_str());
            ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    dl->AddLine(ImVec2(wpos.x + sideW, wpos.y + bodyY), ImVec2(wpos.x + sideW, wpos.y + bodyY + bodyH), col(kGold, 0.18f));

    // ---- the page ----
    {
        ImGui::SetCursorPos(ImVec2(sideW, bodyY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, em * 0.9f));
        ImGui::BeginChild("##content", ImVec2(W - sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        ImGui::PopStyleVar();
        if (scrollFrames_ > 0) {
            --scrollFrames_;
            ImGui::SetScrollY(scrollFrac_ * ImGui::GetScrollMaxY());
        }
        const float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize * 0.5f;
        const int columns = (width > em * 62.0f && !searching) ? 2 : 1;

        // restart-required banner (every restart setting whose value differs from the one this session started with)
        {
            std::vector<settings::Setting*> pend;
            for (settings::Setting* s : reg.all()) {
                if (s->restartPending()) pend.push_back(s);
            }
            if (!pend.empty()) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float lineH = em * 1.35f;
                const float boxH = em * 0.9f + lineH * static_cast<float>(pend.size() + 1);
                const float bw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize * 0.5f;
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                cdl->AddRectFilled(p, ImVec2(p.x + bw, p.y + boxH), col(kAmber, 0.10f), em * 0.4f);
                cdl->AddRect(p, ImVec2(p.x + bw, p.y + boxH), col(kAmber, 0.65f), em * 0.4f);
                float y = p.y + em * 0.45f;
                ImGui::PushFont(g_semi, baseSize);
                cdl->AddText(ImVec2(p.x + em, y), col(kAmber), "Restart required");
                ImGui::PopFont();
                y += lineH;
                for (const settings::Setting* s : pend) {
                    const std::string line = s->spec().label + ":  " + displayValue(s->spec(), s->startValue()) + "  ->  " + displayValue(s->spec(), s->value());
                    cdl->AddText(ImVec2(p.x + em, y), col(kText), line.c_str());
                    y += lineH;
                }
                const float bwBtn = em * 11.0f;
                ImGui::SetCursorScreenPos(ImVec2(p.x + bw - bwBtn - em * 0.7f, p.y + em * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Button, kGoldDim);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGold);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGold);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.03f, 1.0f));
                if (ImGui::Button("Apply and restart", ImVec2(bwBtn, frameH))) {
                    std::string err;
                    reg.save(options_.settingsFile, &err);
                    relaunchGame(window_, options_.settingsFile);
                }
                ImGui::PopStyleColor(4);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Saves the settings, closes the game and starts it again with the same command line.");
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + boxH));
                ImGui::Dummy(ImVec2(1, em * 0.5f));
            }
        }
        const std::string title = searching ? "Search" : g_ui.page;
        ImGui::PushFont(g_serif, baseSize * 2.0f);
        ImGui::TextColored(ImVec4(0.96f, 0.88f, 0.66f, 1.0f), "%s", title.c_str());
        ImGui::PopFont();
        if (searching) {
            ImGui::TextDisabled("Settings matching \"%s\" on every page.", g_ui.search);
        } else {
            ImGui::TextDisabled("%s", pageBlurb(g_ui.page));
        }
        ImGui::Dummy(ImVec2(1, em * 0.2f));

        const auto emptyNote = [&](const char* text) { ImGui::Dummy(ImVec2(1, em)); ImGui::TextDisabled("%s", text); };
        if (searching) {
            const auto words = searchWords(g_ui.search);
            if (reg.all().empty()) emptyNote("Nothing to search.");
            bool anyHit = false;
            std::vector<std::string> order = settings::pageOrder();
            for (const std::string& pg : order) {
                const auto sections = collectSections(reg, view, [&](const settings::Spec& sp) { return settings::pageOf(sp) == pg && matches(sp, words); }, false);
                if (sections.empty()) continue;
                anyHit = true;
                ImGui::Dummy(ImVec2(1, em * 0.6f));
                ImGui::PushFont(g_serif, baseSize * 1.3f);
                ImGui::TextColored(ImVec4(0.96f, 0.88f, 0.66f, 1.0f), "%s", pg.c_str());
                ImGui::PopFont();
                for (const Section& sec : sections) {
                    sectionHeading(sec.name, width);
                    drawRows(reg, sec.items, width, 1, view.debug);
                }
            }
            if (!anyHit) emptyNote(view.advanced ? "Nothing matches." : "Nothing matches. Turn on Advanced to search the fine-tuning settings too.");
        } else if (g_ui.page == kPagePerformance) {
            buildPerformancePanel(view.debug);
        } else {
            if (g_ui.page == settings::kPageDebug) buildAbPanel();
            const std::string page = g_ui.page;
            const auto sections = collectSections(reg, view, [&](const settings::Spec& sp) { return settings::pageOf(sp) == page; }, false);
            for (const Section& sec : sections) {
                sectionHeading(sec.name, width);
                drawRows(reg, sec.items, width, sec.name == "Render mode" ? 1 : columns, view.debug);
                if (page == settings::kPageDisplay && sec.name == "Render mode") renderModeNotes(r.rendererMode);
                if (page == settings::kPageDisplay && sec.name == "Draw distances") ImGui::TextDisabled("1.0x = the game's own distances, 2.0x = twice as far. Farther costs some frame rate in forests and towns.");
            }
            if (page == settings::kPageDisplay) {
                ImGui::Dummy(ImVec2(1, em * 0.6f));
                ImGui::TextDisabled("Resolution, brightness and the game's own graphics options: the game's Options menu.");
            } else if (page == settings::kPageSystem) {
                ImGui::Dummy(ImVec2(1, em * 0.6f));
                buildCapabilitiesPanel();
            } else if (page == settings::kPageDebug) {
                buildFramePanel();
            }
        }
        ImGui::Dummy(ImVec2(1, em * 0.5f));
        {
            // rows scrolled under the footer fade out instead of being cut mid-row
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const ImVec2 cs = ImGui::GetWindowSize();
            const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            const ImU32 clear = ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 0.0f)), solid = ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 1.0f));
            if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.0f) {
                cdl->AddRectFilledMultiColor(ImVec2(cp.x, cp.y + cs.y - em * 2.6f), ImVec2(cp.x + cs.x - ImGui::GetStyle().ScrollbarSize, cp.y + cs.y), clear, clear, solid, solid);
            }
            if (ImGui::GetScrollY() > 1.0f) {
                cdl->AddRectFilledMultiColor(cp, ImVec2(cp.x + cs.x - ImGui::GetStyle().ScrollbarSize, cp.y + em * 1.4f), solid, solid, clear, clear);
            }
        }
        ImGui::EndChild();
    }

    // ---- footer ----
    {
        const float fy = H - footerH;
        dl->AddRectFilled(ImVec2(wpos.x + 1, wpos.y + fy), ImVec2(wpos.x + W - 1, wpos.y + H - 1), col(kPanel, 0.9f), 8.0f, ImDrawFlags_RoundCornersBottom);
        dl->AddLine(ImVec2(wpos.x, wpos.y + fy), ImVec2(wpos.x + W, wpos.y + fy), col(kGold, 0.25f));
        const float cy = fy + (footerH - frameH) * 0.5f;
        const float applyW = em * 7.0f, restoreW = em * 10.0f;
        float x = W - pad - applyW;
        ImGui::SetCursorPos(ImVec2(x, cy));
        ImGui::PushStyleColor(ImGuiCol_Button, kGoldDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGold);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGold);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.03f, 1.0f));
        if (ImGui::Button("Apply", ImVec2(applyW, frameH))) setOpen(false);
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Changes already apply as you make them and are saved. This closes the menu.");
        x -= em * 0.8f + restoreW;
        ImGui::SetCursorPos(ImVec2(x, cy));
        const bool canRestore = !searching && g_ui.page != kPagePerformance;
        if (!canRestore) ImGui::BeginDisabled();
        if (ImGui::Button("Restore Defaults", ImVec2(restoreW, frameH))) {
            const std::size_t n = settings::resetPage(reg, g_ui.page);
            spdlog::info("overlay: {} page reset to defaults ({} settings)", g_ui.page, n);
        }
        if (!canRestore) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", canRestore ? "Resets every setting of this page to its default." : "Open a settings page to restore its defaults.");
        }
        // fps + VRAM
        x -= em * 1.4f + em * 12.0f;
        ImGui::SetCursorPos(ImVec2(x, cy));
        buildVramBar(em * 9.0f, true);
        {
            float sum = 0.0f;
            const std::size_t n = std::min<std::size_t>(historyCount_, 60);
            for (std::size_t i = 0; i < n; ++i) sum += intervals_[(historyPos_ + kHistory - 1 - i) % kHistory];
            const float fps = n > 0 && sum > 0.0f ? 1000.0f * static_cast<float>(n) / sum : 0.0f;
            const std::string t = std::format("{:.0f} fps", fps);
            x -= em * 0.9f + ImGui::CalcTextSize(t.c_str()).x;
            ImGui::SetCursorPos(ImVec2(x, cy));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kGold, "%s", t.c_str());
        }
        // status
        ImGui::SetCursorPos(ImVec2(pad, cy));
        ImGui::AlignTextToFramePadding();
        std::size_t pending = 0;
        for (const settings::Setting* s : reg.all()) pending += s->restartPending() ? 1 : 0;
        if (pending > 0) {
            ImGui::TextColored(kAmber, "%zu setting%s need%s a restart of the game.", pending, pending == 1 ? "" : "s", pending == 1 ? "s" : "");
        } else {
            ImGui::TextDisabled("%s  |  %s: close%s", reg.dirty() ? "saving..." : "Saved in rvk\\settings.toml", keyLabel(r.toggleKey).c_str(),
                                view.debug ? std::format("  |  {}: A/B", keyLabel(r.abKey)).c_str() : "");
        }
    }
    // toast: "Also switched on: X"
    if (std::chrono::steady_clock::now() < g_ui.toastUntil) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(g_ui.toast.c_str());
        const float cx = wpos.x + sideW + (W - sideW) * 0.5f, by = wpos.y + H - footerH - em * 1.2f;
        const ImVec2 a(cx - ts.x * 0.5f - em, by - ts.y - em * 0.5f), b(cx + ts.x * 0.5f + em, by + em * 0.5f);
        fg->AddRectFilled(a, b, col(ImVec4(0.14f, 0.11f, 0.07f, 1.0f), 0.97f), em * 0.5f);
        fg->AddRect(a, b, col(kGold, 0.8f), em * 0.5f);
        fg->AddText(ImVec2(a.x + em, a.y + em * 0.5f - ImGui::GetStyle().ItemSpacing.y * 0.0f), col(kText), g_ui.toast.c_str());
    }
    // Escape: clears the search, then closes
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (g_ui.search[0] != 0) g_ui.search[0] = 0;
        else setOpen(false);
    }
    ImGui::End();
}

// ---- panels ---------------------------------------------------------------------------------------------------------

namespace {
const char* sideName(bool b) { return b ? "B" : "A"; }
}  // namespace

void Overlay::buildAbPanel() {
    auto& reg = settings::registry();
    const auto& r = settings::renderer();
    std::string key = r.abKey != nullptr ? upper(r.abKey->asEnum()) : "?";
    const bool b = reg.bActive();
    sectionHeading("A/B comparison", ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted("Active side:");
    ImGui::SameLine();
    ImGui::TextColored(b ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "%s", sideName(b));
    ImGui::SameLine();
    if (ImGui::Button(std::format("Flip to {} ({})", sideName(!b), key).c_str())) reg.flipAB();
    ImGui::SameLine();
    if (ImGui::Button(std::format("Copy {} to {}", sideName(b), sideName(!b)).c_str())) reg.copyActiveToOther();
    ImGui::SameLine();
    if (ImGui::Button(std::format("Reset {} to defaults", sideName(!b)).c_str())) reg.resetOther();
    const auto diffs = reg.abDifferences();
    if (diffs.empty()) {
        ImGui::TextDisabled("Both sides are the same. Side B starts as the defaults:\n"
                            "change settings on one side, then flip to compare.");
    } else if (ImGui::BeginTable("##ab", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        for (const settings::Setting* s : diffs) {
            const double active = s->value(), other = reg.otherValue(*s);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s->spec().label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(displayValue(s->spec(), b ? other : active).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(displayValue(s->spec(), b ? active : other).c_str());
        }
        ImGui::EndTable();
    }
}

// ---- capabilities (core/caps.h, docs/research/raytracing.md 3) ----
void Overlay::buildCapabilitiesPanel() {
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.4f));
    const bool capsOpen = ImGui::TreeNodeEx("Graphics card details", ImGuiTreeNodeFlags_SpanAvailWidth);
    if (!capsOpen) return;
    struct Pop { ~Pop() { ImGui::TreePop(); } } popTree;
    const caps::Report report = settings::registry().capabilities();
    if (!report.known) {
        ImGui::TextDisabled("no Vulkan device");
        return;
    }
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
                ImGui::TextColored(kGreen, "enabled");
            } else {
                ImGui::TextColored(e.available ? kAmber : kTextDim, "%s", e.available ? "available, off" : "unavailable");
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

// ---- performance page; `debug` adds the diagnostics (present mode, pacing, enhancement status lines) ----
void Overlay::buildPerformancePanel(bool debug) {
    const float em = ImGui::GetFontSize();
    const float width = ImGui::GetContentRegionAvail().x;
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
    sectionHeading("Frame rate", width);
    ImGui::PushFont(g_serif, ImGui::GetStyle().FontSizeBase * 2.4f);
    ImGui::TextColored(kGold, "%.0f", avg > 0.0f ? 1000.0f / avg : 0.0f);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("fps   frame %.2f ms (worst %.2f ms over %zu frames)", avg, worst, n);
    {
        const d3d9::Device::LiveFrame& lf = device_->liveFrame();
        if (lf.outputFps > 0.0f) {
            ImGui::Text("Frame generation: %.0f rendered -> %.0f shown fps (the frame time is the rendered one; about +%.0f ms input delay)",
                        lf.baseFps, lf.outputFps, lf.baseFps > 0.0f ? 1000.0f / lf.baseFps : 0.0f);
        }
    }
    ImGui::Text("CPU (engine + renderer) %.2f ms", n > 0 ? cpuSum / static_cast<float>(n) : 0.0f);
    if (gpuN > 0) {
        ImGui::Text("GPU %.2f ms", gpuSum / static_cast<float>(gpuN));
    } else if (debug) {
        ImGui::TextDisabled("GPU time: off (config.toml [debug] perf = true)");
    }
    float ordered[kHistory];
    for (std::size_t i = 0; i < n; ++i) ordered[i] = intervals_[(historyPos_ + kHistory - n + i) % kHistory];
    ImGui::PlotLines("##frametimes", ordered, static_cast<int>(n), 0, "frame time (ms)", 0.0f, std::max(20.0f, worst * 1.1f), ImVec2(width, em * 5.0f));
    sectionHeading("Video memory", width);
    buildVramBar(std::min(width, em * 28.0f), true);
    sectionHeading("Output", width);
    const auto& st = device_->stats();
    ImGui::Text("%u draws, vsync %s, frame rate limit %s", st.lastFrameDraws, device_->vsync() ? "on" : "off",
                device_->maxFps() > 0.0 ? std::format("{:.0f} fps", device_->maxFps()).c_str() : "none");
    ImGui::TextUnformatted(engine::window::displaySummary().c_str());
    if (debug) {
        ImGui::Text("present mode %d%s, frame pacing %.1f fps", static_cast<int>(device_->presentMode()),
                    device_->fullScreenExclusiveHeld()         ? ", Vulkan exclusive fullscreen held"
                    : device_->fullScreenExclusiveConfigured() ? ", exclusive swapchain (not held)"
                                                               : "",
                    device_->pacerRate());
        for (const auto& [key, text] : device_->status()) ImGui::TextUnformatted(text.c_str());
    }
}

// ---- the last frame's phases and camera (d3d9/scene.h, docs/research/frame-phases.md; debug mode) ----
void Overlay::buildFramePanel() {
    if (!ImGui::CollapsingHeader("Frame structure")) return;
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
        ImGui::Text("camera (%.1f, %.1f, %.1f), near %.1f, far %.0f", c.position[0], c.position[1], c.position[2], c.nearZ, c.engineFar);
    } else {
        ImGui::TextDisabled("no main view in the last frame");
    }
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

}  // namespace rvk::overlay
