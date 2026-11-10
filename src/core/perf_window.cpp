#include "core/perf_window.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <mutex>

#include <spdlog/spdlog.h>

namespace rvk::perfwin {

namespace {

struct Acc {
    double sum = 0.0, sumSq = 0.0;
    void add(double v) {
        sum += v;
        sumSq += v * v;
    }
    double mean(double n) const { return n > 0 ? sum / n : 0.0; }
    double sd(double n) const {
        if (n < 2) return 0.0;
        const double m = sum / n;
        return std::sqrt(std::max(0.0, (sumSq - n * m * m) / (n - 1)));
    }
};

struct PassAcc {
    Acc ms;
    std::uint64_t frames = 0;  // frames that had the key
    double draws = 0.0;
};

struct Window {
    std::string tag;
    std::uint32_t frames = 0;
    std::uint64_t first = 0, last = 0;  // recorded frame numbers (0 = not started)
    bool ended = false;                 // its last frame was presented
    std::uint64_t timed = 0;            // GPU results received
    Acc main, uploads;
    std::map<std::string, PassAcc> passes;
};

std::mutex g_mutex;
std::uint32_t g_armedFrames = 0;  // a window waiting for its first frame
std::string g_armedTag;
Window* g_current = nullptr;         // recording (first set, not ended)
std::vector<Window*> g_collecting;   // started windows whose GPU results are still coming in
std::uint64_t g_lastPresented = 0;

void finish(Window* w) {
    const double n = static_cast<double>(w->timed);
    spdlog::info("perf: window {} gpu: {} frames timed (of {}), main mean {:.4f} ms sd {:.4f}, uploads mean {:.4f} ms", w->tag, w->timed, w->frames,
                 w->main.mean(n), w->main.sd(n), w->uploads.mean(n));
    // passes by mean time, largest first
    std::vector<const std::pair<const std::string, PassAcc>*> order;
    for (const auto& p : w->passes) order.push_back(&p);
    std::ranges::sort(order, [](auto* a, auto* b) { return a->second.ms.sum > b->second.ms.sum; });
    for (const auto* p : order) {
        // (a key missing in a frame counted 0 there: sum of squares over the frames that had it, the others add 0)
        spdlog::info("perf: window {} pass mean {:.4f} ms sd {:.4f} n {} draws {:.1f} | {}", w->tag, p->second.ms.mean(n), p->second.ms.sd(n),
                     p->second.frames, p->second.frames > 0 ? p->second.draws / static_cast<double>(p->second.frames) : 0.0, p->first);
    }
    delete w;
}

// windows whose results cannot all come back any more (16 presents after the end): summarised with what arrived
void reap() {
    for (auto it = g_collecting.begin(); it != g_collecting.end();) {
        Window* w = *it;
        if (w->ended && (w->timed >= w->frames || g_lastPresented > w->last + 16)) {
            it = g_collecting.erase(it);
            finish(w);
        } else {
            ++it;
        }
    }
}

}  // namespace

std::string passKey(std::string_view label, std::uint32_t* draws) {
    if (draws != nullptr) *draws = 0;
    constexpr std::string_view kBarriers = "barriers / copies / inline uploads before pass ";
    if (label.starts_with(kBarriers)) return "barriers / copies / inline uploads";
    if (label.starts_with("pass ") && label.size() > 5 && label[5] >= '0' && label[5] <= '9') {
        const auto colon = label.find(": ");
        if (colon != std::string_view::npos) {
            std::string_view rest = label.substr(colon + 2);
            const auto comma = rest.rfind(", ");
            if (comma != std::string_view::npos && rest.ends_with(" draws")) {
                const std::string_view n = rest.substr(comma + 2, rest.size() - comma - 2 - 6);
                std::uint32_t d = 0;
                const auto [p, ec] = std::from_chars(n.data(), n.data() + n.size(), d);
                if (ec == std::errc() && p == n.data() + n.size()) {
                    if (draws != nullptr) *draws = d;
                    rest = rest.substr(0, comma);
                }
            }
            return std::string(rest);
        }
    }
    return std::string(label);
}

bool arm(std::uint32_t frames, std::string_view tag, std::string* why) {
    const std::lock_guard lock(g_mutex);
    if (frames == 0 || frames > 100000 || tag.empty() || tag.find(' ') != std::string_view::npos) {
        if (why != nullptr) *why = "perf window <frames 1..100000> <tag without spaces>";
        return false;
    }
    if (g_armedFrames != 0 || g_current != nullptr) {
        if (why != nullptr) *why = std::format("window {} is still {}", g_current ? g_current->tag : g_armedTag, g_current ? "running" : "armed");
        return false;
    }
    g_armedFrames = frames;
    g_armedTag = std::string(tag);
    if (why != nullptr) *why = std::format("window {} armed: {} frames from the next recorded frame", tag, frames);
    return true;
}

bool beginFrame(std::uint64_t frame) {
    const std::lock_guard lock(g_mutex);
    if (g_current == nullptr && g_armedFrames != 0) {
        auto* w = new Window;
        w->tag = std::move(g_armedTag);
        w->frames = g_armedFrames;
        w->first = frame;
        w->last = frame + g_armedFrames - 1;
        g_armedFrames = 0;
        g_current = w;
        g_collecting.push_back(w);
    }
    return g_current != nullptr && frame >= g_current->first && frame <= g_current->last;
}

Edge afterPresent(std::uint64_t presented, std::string* tag) {
    const std::lock_guard lock(g_mutex);
    g_lastPresented = presented;
    Edge e = Edge::None;
    if (g_current != nullptr) {
        if (presented + 1 == g_current->first) {
            e = Edge::Begin;
            if (tag != nullptr) *tag = g_current->tag;
        } else if (presented >= g_current->last) {
            e = Edge::End;
            if (tag != nullptr) *tag = g_current->tag;
            g_current->ended = true;
            g_current = nullptr;
        }
    }
    reap();
    return e;
}

bool gpuFrame(std::uint64_t frame, double mainMs, double uploadMs, const std::vector<Segment>& segments) {
    const std::lock_guard lock(g_mutex);
    Window* w = nullptr;
    for (Window* c : g_collecting) {
        if (frame >= c->first && frame <= c->last) w = c;
    }
    if (w == nullptr) return false;
    ++w->timed;
    w->main.add(mainMs);
    w->uploads.add(uploadMs);
    std::map<std::string, std::pair<double, std::uint32_t>> perFrame;
    for (const Segment& s : segments) {
        std::uint32_t d = 0;
        auto& slot = perFrame[passKey(s.label, &d)];
        slot.first += s.ms;
        slot.second += d;
    }
    for (const auto& [k, v] : perFrame) {
        PassAcc& p = w->passes[k];
        p.ms.add(v.first);
        ++p.frames;
        p.draws += v.second;
    }
    reap();
    return true;
}

bool running() {
    const std::lock_guard lock(g_mutex);
    return g_current != nullptr;
}

void reset() {
    const std::lock_guard lock(g_mutex);
    for (Window* w : g_collecting) delete w;
    g_collecting.clear();
    g_current = nullptr;
    g_armedFrames = 0;
    g_armedTag.clear();
    g_lastPresented = 0;
}

}  // namespace rvk::perfwin
