#pragma once

// Injection points (roadmap do-now item 6, docs/research/injection.md): places in the engine's frame where enhancement passes
// record their own commands (compute, fullscreen passes, copies) into the frame's command buffer. The device leaves any open
// rendering scope first and restores its own recording state afterwards, so a pass sees no D3D9 state and the D3D9 draws
// after it see none of the pass's. With nothing registered at a point, firing it costs one bit test.
//
// The registry is pure logic (unit tests: tests/inject_tests.cpp); Device::inject (d3d9/device.h) runs it. Render thread.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rvk::d3d9 {
struct PassContext;  // d3d9/device.h
}

namespace rvk::d3d9::inject {

// Where (in engine order within a frame; frame-phases.md 3). The Main points fire once per main view.
enum class Point : std::uint8_t {
    BeforeDepthPrepass,   // Main view, BeginZPass: the depth map (R32F) and the hardware depth are bound and cleared, the engine's z pass has drawn nothing yet
    AfterDepthPrepass,    // Main view, EndZPass: the linear depth map (R32F, view depth / engine far) is complete
    BeforeSolid,          // Main view, DoBeginRenderSolid has returned: the scene HDR target and the hardware depth (the z pass's) are bound, the engine's solid
                          // draws (solid ops incl. blended terrain layers, decals, billboards, then the sky dome last) have not started: what is drawn here lies
                          // under all of them (engine-frame/B-frustum-db.md 2.6)
    AfterOpaque,          // Main view, end of Solid: before the refraction copy (or at DoBeginRenderAlpha without one). The
                          // scene HDR target holds the opaque scene (sky, solid, decals); water and alpha are not drawn yet
    AfterRefractionCopy,  // Main view, right after the refraction copy (CopyCurrentRTToImage), before the refracted (water) draws
    BeforeAlpha,          // Main view, just before the first draw of the alpha passes (Alpha / AlphaLate): the opaque and refracted
                          // scene (sky, solid, decals, water) is in the scene HDR target, no translucent effect (particles, rain,
                          // spells, spray) is drawn yet. Fired from the draw path (Device::prepareDraw), once per main view
    AfterMainView,        // the main view's eCFrustumDatabase::Execute returns: the scene HDR target is complete (incl. water,
                          // alpha), the image-effect post chain (eye adaptation, tonemapping, DoF) has not run yet
    BeforeGui,            // eCApplication::Draw: the post chain is done (the back buffer holds the tonemapped frame), no GUI yet
    Count
};
inline constexpr std::size_t kPointCount = static_cast<std::size_t>(Point::Count);
const char* name(Point p);
// whether a point belongs to the main view (fired at most once per main view)
constexpr bool inMainView(Point p) { return p != Point::BeforeGui; }

using PassFn = std::function<void(PassContext&)>;

class Registry {
public:
    struct Entry {
        std::uint32_t id = 0;
        Point point = Point::AfterOpaque;
        int order = 0;  // lower first; equal orders run in registration order
        std::string name;
        const void* owner = nullptr;
        PassFn fn;
    };

    // returns the pass id (never 0)
    std::uint32_t add(Point point, std::string name, PassFn fn, const void* owner = nullptr, int order = 0);
    bool remove(std::uint32_t id);
    std::size_t removeOwner(const void* owner);
    bool any(Point p) const { return (mask_ & bit(p)) != 0; }
    bool empty() const { return mask_ == 0; }
    // the passes of a point in execution order (valid until the next add / remove)
    const std::vector<Entry>& passes(Point p) const { return lists_[static_cast<std::size_t>(p)]; }
    std::size_t size() const;

private:
    static std::uint32_t bit(Point p) { return 1u << static_cast<std::uint32_t>(p); }
    void updateMask();
    std::array<std::vector<Entry>, kPointCount> lists_;
    std::uint32_t mask_ = 0;
    std::uint32_t nextId_ = 1;
};

// Which main-view points fired in the current main view (AfterOpaque has two triggers: the refraction copy, else the
// start of the alpha pass). Reset when a main view begins.
class Firings {
public:
    void beginMainView() { fired_ = 0; ++mainViews_; }
    void endFrame() { fired_ = 0; mainViews_ = 0; }
    // true when `p` should run now (and marks it); main-view points only inside a main view, once each
    bool take(Point p, bool inMain);
    std::uint32_t mainViews() const { return mainViews_; }  // main views begun this frame

private:
    std::uint32_t fired_ = 0;
    std::uint32_t mainViews_ = 0;
};

}  // namespace rvk::d3d9::inject
