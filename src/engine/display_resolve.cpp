#include "engine/display_resolve.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

namespace rvk::engine::window {
namespace {

bool hasSize(const std::vector<DisplayMode>& modes, std::uint32_t w, std::uint32_t h) {
    return std::ranges::any_of(modes, [&](const DisplayMode& m) { return m.width == w && m.height == h; });
}

double aspect(std::uint32_t w, std::uint32_t h) { return h != 0 ? static_cast<double>(w) / h : 0.0; }

}  // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Windowed: return "windowed";
        case Kind::Borderless: return "borderless";
        case Kind::Exclusive: return "exclusive";
    }
    return "?";
}

std::uint32_t pickRefresh(const std::vector<DisplayMode>& modes, std::uint32_t width, std::uint32_t height, std::uint32_t refresh) {
    if (refresh == 0) return 0;
    std::uint32_t below = 0, above = 0;
    for (const DisplayMode& m : modes) {
        if (m.width != width || m.height != height) continue;
        if (m.refresh == refresh) return refresh;
        if (m.refresh < refresh) {
            below = std::max(below, m.refresh);
        } else {
            above = above == 0 ? m.refresh : std::min(above, m.refresh);
        }
    }
    if (below != 0) return below;
    if (above != 0) return above;
    return refresh;  // the size is not in the list at all
}

Resolved resolve(const Request& req, const MonitorInfo& mon) {
    Resolved out;
    out.width = req.width;
    out.height = req.height;
    out.refresh = req.refresh;
    out.kind = req.kind;
    const std::uint32_t dw = mon.desktop.width != 0 ? mon.desktop.width : static_cast<std::uint32_t>(std::max(mon.rect.width, 0));
    const std::uint32_t dh = mon.desktop.height != 0 ? mon.desktop.height : static_cast<std::uint32_t>(std::max(mon.rect.height, 0));
    if (dw == 0 || dh == 0 || req.width == 0 || req.height == 0) return out;  // no monitor information: unchanged
    const std::string where = mon.name.empty() ? std::string("the monitor") : mon.name;
    std::string why;

    const auto borderless = [&](const std::string& cause) {
        out.kind = Kind::Borderless;
        out.width = dw;
        out.height = dh;
        why = std::format("{}: borderless at the monitor's {}x{}", cause, dw, dh);
    };

    switch (req.kind) {
        case Kind::Borderless:
            if (req.width != dw || req.height != dh) {
                out.width = dw;
                out.height = dh;
                why = std::format("borderless covers the monitor: its desktop {}x{}", dw, dh);
            }
            break;
        case Kind::Exclusive: {
            if (!hasSize(mon.modes, req.width, req.height)) {
                // best match: same aspect first, nearest area, not above the desktop, then the larger one
                const double ar = aspect(req.width, req.height);
                const double want = static_cast<double>(req.width) * req.height;
                const DisplayMode* best = nullptr;
                const auto key = [&](const DisplayMode& m) {
                    const bool sameAspect = std::abs(aspect(m.width, m.height) - ar) <= ar * 0.01;
                    const double areaDiff = std::abs(static_cast<double>(m.width) * m.height - want);
                    return std::pair(sameAspect ? 0 : 1, areaDiff);
                };
                for (const DisplayMode& m : mon.modes) {
                    if (m.width > dw || m.height > dh || m.width < kMinWidth || m.height < kMinHeight) continue;
                    if (best == nullptr || key(m) < key(*best) ||
                        (key(m) == key(*best) && std::pair(m.width, m.height) > std::pair(best->width, best->height))) {
                        best = &m;
                    }
                }
                if (best != nullptr) {
                    out.width = best->width;
                    out.height = best->height;
                    why = std::format("exclusive mode {}x{} is not offered: the nearest one", req.width, req.height);
                } else {
                    out.width = dw;
                    out.height = dh;
                    why = std::format("exclusive mode {}x{} is not offered: the desktop mode", req.width, req.height);
                }
            }
            out.refresh = pickRefresh(mon.modes, out.width, out.height, req.refresh);
            break;
        }
        case Kind::Windowed: {
            if (req.width >= dw && req.height >= dh) {
                borderless(req.width == dw && req.height == dh ? std::string("windowed at the monitor's size")
                                                               : std::format("windowed {}x{} does not fit {}", req.width, req.height, where));
                break;
            }
            if (req.width > dw || req.height > dh) {
                borderless(std::format("windowed {}x{} does not fit {}", req.width, req.height, where));
                break;
            }
            const int maxW = std::max<int>(static_cast<int>(kMinWidth), mon.work.width - mon.frameWidth);
            const int maxH = std::max<int>(static_cast<int>(kMinHeight), mon.work.height - mon.frameHeight);
            std::uint32_t w = req.width, h = req.height;
            if (mon.work.width > 0 && (static_cast<int>(w) > maxW || static_cast<int>(h) > maxH)) {
                const double s = std::min(static_cast<double>(maxW) / w, static_cast<double>(maxH) / h);
                w = static_cast<std::uint32_t>(std::floor(w * s)) & ~1u;
                h = static_cast<std::uint32_t>(std::lround(static_cast<double>(w) * req.height / req.width)) & ~1u;
                // prefer a display mode of the same aspect that fits (crisp, listed by the options menu)
                const double ar = aspect(req.width, req.height);
                const DisplayMode* fit = nullptr;
                for (const DisplayMode& m : mon.modes) {
                    if (static_cast<int>(m.width) > maxW || static_cast<int>(m.height) > maxH || m.width < kMinWidth) continue;
                    if (std::abs(aspect(m.width, m.height) - ar) > ar * 0.01) continue;
                    if (fit == nullptr || m.width > fit->width) fit = &m;
                }
                if (fit != nullptr) {
                    w = fit->width;
                    h = fit->height;
                }
                why = std::format("windowed {}x{} does not fit the work area of {} ({}x{})", req.width, req.height, where, maxW, maxH);
            }
            if (w < kMinWidth || h < kMinHeight) {
                w = std::max(w, kMinWidth);
                h = std::max(h, kMinHeight);
                if (why.empty()) why = std::format("windowed {}x{} is below the minimum", req.width, req.height);
            }
            out.width = w;
            out.height = h;
            // a windowed size that is no display mode keeps the engine's refresh
            if (hasSize(mon.modes, w, h)) out.refresh = pickRefresh(mon.modes, w, h, req.refresh);
            break;
        }
    }
    if (out.kind == Kind::Borderless && out.refresh != 0) out.refresh = pickRefresh(mon.modes, out.width, out.height, out.refresh);
    out.changed = out.width != req.width || out.height != req.height || out.refresh != req.refresh || out.kind != req.kind;
    if (out.changed) {
        out.reason = why.empty() ? std::format("refresh {} Hz is not offered at {}x{}", req.refresh, out.width, out.height) : why;
    }
    return out;
}

std::vector<DisplayMode> engineModes(std::vector<DisplayMode> modes, const Resolved& r) {
    if (r.width != 0 && r.height != 0 &&
        !std::ranges::any_of(modes, [&](const DisplayMode& m) {
            return m.width == r.width && m.height == r.height && (r.refresh == 0 || m.refresh == r.refresh);
        })) {
        // a windowed size that is no display mode (or a refresh the size lacks): the engine's exact match needs it
        modes.push_back({r.width, r.height, r.refresh != 0 ? r.refresh : (modes.empty() ? 60u : modes.front().refresh)});
    }
    return modes;
}

}  // namespace rvk::engine::window
