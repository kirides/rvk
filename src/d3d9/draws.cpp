#include "d3d9/draws.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace rvk::d3d9::draws {
namespace {

constexpr const char* kSourceNames[] = {"None",    "Mesh",     "Skinned", "SpeedTree", "SpeedTreeBillboard", "Vegetation",
                                        "Debug",   "Sky",      "Particle", "Strip",    "Decal",              "Billboard", "Precipitation"};
static_assert(std::size(kSourceNames) == static_cast<std::size_t>(Source::Count));

constexpr const char* kKindNames[] = {"Unknown",  "StaticMesh", "InstancedMesh", "Refracted", "Skinned", "SpeedTree",
                                      "SpeedTreeBillboard", "Vegetation", "Sky", "Particle", "Strip", "Decal",
                                      "Billboard", "Precipitation", "Debug", "ShadowMask", "Post", "Gui"};
static_assert(std::size(kKindNames) == static_cast<std::size_t>(Kind::Count));

constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a = 220) {
    return r | g << 8 | b << 16 | a << 24;
}

// one colour per kind, chosen to stay apart (docs/research/draw-tags.md 5 has the legend)
constexpr std::uint32_t kKindTint[] = {
    rgba(255, 0, 255),    // Unknown: magenta
    rgba(190, 190, 190),  // StaticMesh: light grey
    rgba(0, 200, 210),    // InstancedMesh: cyan
    rgba(20, 70, 255),    // Refracted: blue
    rgba(255, 30, 30),    // Skinned: red
    rgba(30, 230, 30),    // SpeedTree: green
    rgba(10, 110, 10),    // SpeedTreeBillboard: dark green
    rgba(190, 240, 0),    // Vegetation: yellow-green
    rgba(150, 110, 255),  // Sky: violet
    rgba(255, 150, 0),    // Particle: orange
    rgba(255, 100, 200),  // Strip: pink
    rgba(150, 90, 40),    // Decal: brown
    rgba(255, 235, 0),    // Billboard: yellow
    rgba(120, 200, 255),  // Precipitation: light blue
    rgba(255, 255, 255),  // Debug: white
    0, 0, 0,              // ShadowMask, Post, Gui: never tinted
};
static_assert(std::size(kKindTint) == static_cast<std::size_t>(Kind::Count));

}  // namespace

const char* name(Source s) {
    const auto i = static_cast<std::size_t>(s);
    return i < std::size(kSourceNames) ? kSourceNames[i] : "?";
}

const char* name(Kind k) {
    const auto i = static_cast<std::size_t>(k);
    return i < std::size(kKindNames) ? kKindNames[i] : "?";
}

Kind classify(Source source, std::uint32_t opDesc, scene::View view, scene::Pass pass) {
    switch (source) {
    case Source::Mesh:
        if (pass == scene::Pass::Refracted) return Kind::Refracted;
        return (opDesc >> 8) == 0xffffffu ? Kind::InstancedMesh : Kind::StaticMesh;
    case Source::Skinned: return Kind::Skinned;
    case Source::SpeedTree: return Kind::SpeedTree;
    case Source::SpeedTreeBillboard: return Kind::SpeedTreeBillboard;
    case Source::Vegetation: return Kind::Vegetation;
    case Source::Debug: return Kind::Debug;
    case Source::Sky: return Kind::Sky;
    case Source::Particle: return Kind::Particle;
    case Source::Strip: return Kind::Strip;
    case Source::Decal: return Kind::Decal;
    case Source::Billboard: return Kind::Billboard;
    case Source::Precipitation: return Kind::Precipitation;
    case Source::None:
    case Source::Count: break;
    }
    if (view == scene::View::Gui) return Kind::Gui;
    if (pass == scene::Pass::ShadowMask) return Kind::ShadowMask;
    if (view == scene::View::Post) return Kind::Post;
    return Kind::Unknown;
}

std::uint32_t Histogram::kind(Kind k) const {
    std::uint32_t n = 0;
    for (std::uint32_t c : counts[static_cast<std::size_t>(k)]) n += c;
    return n;
}

std::string Histogram::summary() const {
    std::array<std::size_t, static_cast<std::size_t>(Kind::Count)> order{};
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return kind(static_cast<Kind>(a)) > kind(static_cast<Kind>(b));
    });
    std::string s;
    for (std::size_t k : order) {
        const std::uint32_t n = kind(static_cast<Kind>(k));
        if (n == 0) break;
        if (!s.empty()) s += ", ";
        s += std::format("{} {} [", name(static_cast<Kind>(k)), n);
        bool first = true;
        for (std::size_t v = 0; v < counts[k].size(); ++v) {
            if (counts[k][v] == 0) continue;
            s += std::format("{}{} {}", first ? "" : ", ", scene::name(static_cast<scene::View>(v)), counts[k][v]);
            first = false;
        }
        s += ']';
    }
    return s.empty() ? std::string("(no draws)") : s;
}

Record* FrameDraws::add() {
    if (!recording_) return nullptr;
    if (cur_.size() >= kMaxRecords) {
        ++dropped_;
        return nullptr;
    }
    if (cur_.capacity() == 0) cur_.reserve(kMaxRecords);
    return &cur_.emplace_back();
}

void FrameDraws::endFrame() {
    std::swap(cur_, last_);
    cur_.clear();
    lastHist_ = hist_;
    hist_ = {};
    lastDropped_ = dropped_;
    dropped_ = 0;
}

std::uint32_t tintColor(TintMode mode, Kind kind, scene::View view, scene::Pass pass) {
    using scene::Pass;
    using scene::View;
    if (mode == TintMode::Off) return 0;
    if (view != View::Main && view != View::Preview && view != View::Other) return 0;
    if (pass != Pass::Setup && pass != Pass::Solid && pass != Pass::Refracted && pass != Pass::Alpha && pass != Pass::AlphaLate) {
        return 0;
    }
    if (mode == TintMode::Kind) return kKindTint[static_cast<std::size_t>(kind)];
    switch (pass) {
    case Pass::Solid: return rgba(190, 190, 190);
    case Pass::Refracted: return rgba(20, 70, 255);
    case Pass::Alpha: return rgba(255, 150, 0);
    case Pass::AlphaLate: return rgba(255, 30, 30);
    default: return rgba(255, 0, 255);  // Setup: draws between the passes
    }
}

}  // namespace rvk::d3d9::draws
