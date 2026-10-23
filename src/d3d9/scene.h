#pragma once

// Frame semantics on top of the D3D9 draw stream (docs/research/frame-phases.md): which view and pass of the engine's frame
// is being rendered (phase markers, driven by detours on engine functions: src/engine/scene_hooks.*), the roles of engine
// images (scene HDR target, refraction copy, depth map, sky cube, shadow maps) and the main camera of each frame.
// Pure logic, no Vulkan and no engine access (unit tests: tests/scene_tests.cpp). Render thread only.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rvk::shader::abi {
struct FrameBlock;
}

namespace rvk::d3d9::scene {

// what the engine renders into: a camera view of the world, a shadow map, the sky's environment cube, an entity preview,
// the image-effect (post) chain, the GUI
enum class View : std::uint8_t { None, Main, Shadow, SkyCube, Preview, Post, Gui, Other, Count };

// where inside the view (eCFrustumDatabase::Execute's order, engine-gfx.md 4.2)
enum class Pass : std::uint8_t {
    None,
    Setup,         // view entry (camera, clear) and the gaps between the passes below
    DepthPrepass,  // BeginZPass .. EndZPass into the linear depth map
    ShadowMask,    // eCIlluminationAdmin::UpdateShadowMask (shadow maps nest as Shadow views, then the screen-space mask)
    ShadowDepth,   // a shadow map's z pass (eCFrustumDatabase::ExecuteShadowMap)
    Solid,         // DoBeginRenderSolid .. DoBeginRenderAlpha: solid ops, debug, decals, billboards, refraction copy
    Refracted,     // eCRenderContextMesh::RenderRefracted ops (water)
    Alpha,         // DoBeginRenderAlpha .. DoEndRenderAlpha: depth-sorted alpha ops
    AlphaLate,     // after DoEndRenderAlpha (list +0x1548)
    Sky,           // eCSkydome_PS::UpdateSkydomeEnvCubemap
    Post,          // eCImageEffect::Execute outside a nested view
    Gui,           // eCApplication::Draw (GUI, snapshot, SwapScreen)
    Count
};

// roles of engine images, set where the engine reveals them (phase hooks, HAL calls); queryable by later passes
enum class ImageRole : std::uint8_t { None, SceneHdr, Refraction, DepthMap, SkyEnvCube, ShadowMap, ShadowCube, Count };

const char* name(View v);
const char* name(Pass p);
const char* name(ImageRole r);

struct Phase {
    View view = View::None;
    Pass pass = Pass::None;
    bool operator==(const Phase&) const = default;
};

// The phase state machine: a stack of view scopes (views nest: the shadow maps inside the main view's shadow-mask pass), the
// pass inside the innermost view, and the frame's sequence of segments (a segment = a stretch of one phase) with their draws.
class PhaseTracker {
public:
    static constexpr std::size_t kMaxDepth = 16;
    static constexpr std::size_t kMaxSegments = 1024;

    struct Segment {
        Phase phase;
        std::uint8_t depth = 0;     // scope depth (0 = outside every view)
        bool enter = false;         // first segment of a view scope
        bool continued = false;     // a scope still open at the frame boundary, re-entered at the next frame's start
        std::uint32_t draws = 0;
    };

    void push(View view, Pass pass);  // enter a view scope
    void pop();                       // leave it: the enclosing phase continues
    void setPass(Pass pass);          // inside the current scope
    Phase current() const { return cur_; }
    View view() const { return cur_.view; }
    Pass pass() const { return cur_.pass; }
    std::size_t depth() const { return stack_.size(); }
    void countDraw();

    // frame boundary (present): the frame's segments become lastFrame(); a scope still open (present inside a view:
    // eCApplication::Draw calls SwapScreen) stays open and starts the next frame's sequence
    void endFrame();
    const std::vector<Segment>& frame() const { return segments_; }
    const std::vector<Segment>& lastFrame() const { return last_; }
    std::uint32_t viewsEntered(View v) const { return entered_[static_cast<std::size_t>(v)]; }  // this frame
    std::uint32_t lastViewsEntered(View v) const { return lastEntered_[static_cast<std::size_t>(v)]; }
    std::uint32_t lastDraws() const { return lastDraws_; }
    std::uint64_t unbalanced() const { return unbalanced_; }  // pops without a push, pushes beyond kMaxDepth

    // one line: "Sky{Sky(6)} Main{DepthPrepass(312) Shadow{ShadowDepth(120)} Solid(640) Refracted(3) Alpha(40)} Post{Post(18)}
    // Gui{Gui(85)}". Passes without draws are left out, adjacent equal passes merged; views always appear, except a scope
    // continued from the previous frame in which nothing happens (the Gui scope SwapScreen leaves open).
    static std::string summary(const std::vector<Segment>& segments);

private:
    void open(bool enter);

    Phase cur_{};
    std::vector<Phase> stack_;  // the enclosing phases of the open scopes
    std::vector<Segment> segments_;
    std::vector<Segment> last_;
    std::array<std::uint32_t, static_cast<std::size_t>(View::Count)> entered_{};
    std::array<std::uint32_t, static_cast<std::size_t>(View::Count)> lastEntered_{};
    std::uint32_t draws_ = 0;
    std::uint32_t lastDraws_ = 0;
    std::uint64_t unbalanced_ = 0;
    bool overflow_ = false;
};

// ---- camera -------------------------------------------------------------------------------------------------------------

// 4x4 float matrix, row-major, D3D row-vector convention (v' = v * M), the layout of the engine's bCMatrix
struct Mat4 {
    std::array<float, 16> m{};
    float& at(int row, int col) { return m[static_cast<std::size_t>(row * 4 + col)]; }
    float at(int row, int col) const { return m[static_cast<std::size_t>(row * 4 + col)]; }
    static Mat4 identity();
};
Mat4 mul(const Mat4& a, const Mat4& b);  // a * b
Mat4 transpose(const Mat4& a);
bool inverse(const Mat4& a, Mat4& out);  // false when singular
float maxAbsDiff(const Mat4& a, const Mat4& b);
// near / far planes of a D3D perspective projection (z/w in 0..1, left- or right-handed); false for anything else
bool nearFar(const Mat4& projection, float& nearZ, float& farZ);

// the engine's camera as read from an eCGfxCamera (view +0x20, projection +0x60, view-projection +0xe0, inverse view +0x120,
// position +0x150) and the far plane of its eCCameraBase (+0xc0, what SetCamera writes into c11.w)
struct CameraInput {
    Mat4 view, projection, viewProjection, inverseView;
    std::array<float, 3> position{};
    float engineFar = 0.0f;
};

// The main view's camera of a frame (roadmap do-now item 10): current and previous view-projection, inverses, position,
// near / far, frame index and time. Filled at the first main view of the frame; the previous-frame fields hold the last
// frame that had a main view (prevValid false after a frame without one, e.g. loading screens).
struct CameraBlock {
    bool valid = false;
    std::uint64_t frame = 0;  // presented-frame index this camera belongs to (rvk_frame_index of its present)
    double time = 0.0;        // seconds since the device was created, at the main view
    Mat4 view, projection, viewProjection, inverseViewProjection, inverseView, inverseProjection;
    std::array<float, 3> position{};
    float nearZ = 0.0f, farZ = 0.0f;  // from the projection
    float engineFar = 0.0f;           // eCCameraBase +0xc0 (c11.w)
    bool prevValid = false;
    Mat4 prevViewProjection, prevInverseViewProjection;
    std::array<float, 3> prevPosition{};
    // the VS constants c0..c11 of the main view's first solid draw (g_vViewProjection c0, g_vInvProjection c4, g_vInvView c8,
    // g_vCamPosAndFar c11): a cross-check of the camera read from the engine
    bool constantsCaptured = false;
    std::array<std::array<float, 4>, 12> vsConstants{};

    void set(const CameraInput& in, std::uint64_t frameIndex, double seconds);
    // next frame: this frame's camera becomes the previous one
    void advance();
};

// How well the harvested VS constants match the camera (all max |difference|; layout = whether c0..c3 hold the
// view-projection's rows or its columns, i.e. the transposed matrix)
struct CameraCheck {
    bool available = false;
    bool transposed = false;
    float viewProjection = 0.0f;     // c0..c3
    float inverseProjection = 0.0f;  // c4..c7
    float inverseView = 0.0f;        // c8..c10 (3 registers)
    float position = 0.0f;           // c11.xyz
    float farPlane = 0.0f;           // c11.w vs engineFar
    float scale = 1.0f;              // largest |element| of the view-projection (to judge the absolute differences)
};
CameraCheck checkCamera(const CameraBlock& cam);

// ---- the GPU frame block (shader/abi.h FrameBlock, roadmap do-now item 5) ------------------------------------------------

// what the frame block holds besides the camera
struct FrameExtras {
    std::uint64_t frameIndex = 0;  // the frame being recorded (CameraBlock::frame numbering)
    double now = 0.0;              // seconds since the device was created (used when there is no camera yet)
    std::uint32_t mainViews = 0;   // main views begun so far this frame
    float width = 0.0f;            // scene target size (0 = unknown)
    float height = 0.0f;
    std::array<std::uint32_t, 4> images{};  // bindless slots: scene HDR, refraction copy, depth map (t2D), sky cube (tCube)
    const std::array<std::array<float, 4>, 16>* params = nullptr;  // feature parameters (null = zeros)
    std::uint64_t rtTlas = 0, rtRecords = 0;  // the ray tracing scene of this submission (0 = none)
    std::uint32_t rtInstances = 0, rtRecordCount = 0;
    std::uint64_t gi = 0;  // the GI volume header of this frame (0 = none)
    std::array<std::uint32_t, 4> ao{};  // FrameBlock::ao (slot 0 = none)
    std::array<std::uint32_t, 4> sgi{};  // FrameBlock::sgi (slot 0 = none)
    std::array<float, 4> jitter{};       // FrameBlock::jitter: this frame's camera jitter (pixels), the previous frame's
};

// The frame block from the current frame's camera `cur` (valid from its main view on) and the last presented frame's `last`.
// Before this frame's main view the camera fields hold `last`'s (flag kFrameCameraPredicted): shadow maps and the sky cube
// render before the main view (frame-phases.md 3). Without either camera the matrices are identities and the flags 0.
void fillFrameBlock(shader::abi::FrameBlock& out, const CameraBlock& cur, const CameraBlock& last, const FrameExtras& extras);

}  // namespace rvk::d3d9::scene
