// Frame semantics (src/d3d9/scene.*): the phase state machine, its per-frame summary, the camera math and the cross-check
// against the engine's camera constants.

#include <cmath>
#include <string>

#include "d3d9/scene.h"
#include "test.h"

using namespace rvk::d3d9::scene;

namespace {

void draws(PhaseTracker& t, int n) {
    for (int i = 0; i < n; ++i) t.countDraw();
}

// the engine's frame as the hooks drive it (frame-phases.md 2): sky cube, main view with depth pre-pass, two shadow maps in
// the shadow-mask pass, solid, refracted, alpha; post chain; GUI (Draw) with the present inside
void engineFrame(PhaseTracker& t) {
    t.push(View::SkyCube, Pass::Sky);
    draws(t, 6);
    t.pop();
    t.push(View::Post, Pass::Post);           // eCImageEffect::Execute
    t.push(View::Main, Pass::Setup);          // OnUpdateOutput -> Execute
    t.setPass(Pass::DepthPrepass);
    draws(t, 10);
    t.setPass(Pass::Setup);                   // EndZPass
    const Pass saved = t.pass();
    t.setPass(Pass::ShadowMask);              // UpdateShadowMask
    t.push(View::Shadow, Pass::ShadowDepth);  // ExecuteShadowMap
    draws(t, 4);
    t.pop();
    t.push(View::Shadow, Pass::ShadowDepth);
    draws(t, 3);
    t.pop();
    draws(t, 1);                              // the screen-space mask
    t.setPass(saved);
    t.setPass(Pass::Solid);
    draws(t, 20);
    t.setPass(Pass::Alpha);                   // DoBeginRenderAlpha
    for (int i = 0; i < 2; ++i) {             // RenderRefracted ops
        t.setPass(Pass::Refracted);
        draws(t, 1);
        t.setPass(Pass::Alpha);
    }
    draws(t, 5);
    t.setPass(Pass::AlphaLate);
    t.pop();                                  // Execute returns
    draws(t, 8);                              // the post filters
    t.pop();
    t.push(View::Gui, Pass::Gui);             // eCApplication::Draw
    draws(t, 12);
    t.endFrame();                             // SwapScreen inside Draw
    t.pop();                                  // Draw returns (next frame)
}

}  // namespace

TEST(scene_phase_summary) {
    PhaseTracker t;
    engineFrame(t);
    const std::string s = PhaseTracker::summary(t.lastFrame());
    CHECK_MSG(s == "SkyCube{Sky(6)} Post{Main{DepthPrepass(10) Shadow{ShadowDepth(4)} Shadow{ShadowDepth(3)} ShadowMask(1) "
                   "Solid(20) Refracted(2) Alpha(5)} Post(8)} Gui{Gui(12)}",
              s);
    CHECK(t.lastDraws() == 6 + 10 + 4 + 3 + 1 + 20 + 2 + 5 + 8 + 12);
    CHECK(t.lastViewsEntered(View::Shadow) == 2);
    CHECK(t.lastViewsEntered(View::Main) == 1);
    CHECK(t.unbalanced() == 0);
    CHECK(t.depth() == 0);
    CHECK(t.current() == (Phase{View::None, Pass::None}));
    // the next frame starts inside the Gui scope SwapScreen left open; empty, it is left out of the summary
    engineFrame(t);
    const std::string s2 = PhaseTracker::summary(t.lastFrame());
    CHECK_MSG(s2 == s, s2);
}

TEST(scene_phase_nesting_restores) {
    PhaseTracker t;
    t.push(View::Main, Pass::Setup);
    t.setPass(Pass::Solid);
    t.push(View::Shadow, Pass::ShadowDepth);
    CHECK(t.view() == View::Shadow);
    CHECK(t.depth() == 2);
    t.pop();
    CHECK(t.current() == (Phase{View::Main, Pass::Solid}));
    t.pop();
    t.pop();  // unbalanced
    CHECK(t.unbalanced() == 1);
    CHECK(t.current() == (Phase{View::None, Pass::None}));
    // draws outside every view still count
    draws(t, 3);
    t.endFrame();
    CHECK(t.lastDraws() == 3);
    CHECK_MSG(PhaseTracker::summary(t.lastFrame()) == "Main{Shadow{}} None(3)", PhaseTracker::summary(t.lastFrame()));
}

TEST(scene_phase_segment_cap) {
    PhaseTracker t;
    t.push(View::Main, Pass::Setup);
    for (std::size_t i = 0; i < PhaseTracker::kMaxSegments + 50; ++i) {
        t.setPass((i & 1) != 0 ? Pass::Solid : Pass::Alpha);
        t.countDraw();
    }
    CHECK(t.frame().size() == PhaseTracker::kMaxSegments);
    t.pop();
    t.endFrame();
    CHECK(t.lastDraws() == PhaseTracker::kMaxSegments + 50);
    CHECK(t.frame().empty());
}

namespace {

Mat4 perspectiveLH(float fovY, float aspect, float n, float f) {
    Mat4 p;
    const float ys = 1.0f / std::tan(fovY / 2.0f);
    p.at(0, 0) = ys / aspect;
    p.at(1, 1) = ys;
    p.at(2, 2) = f / (f - n);
    p.at(2, 3) = 1.0f;
    p.at(3, 2) = -n * f / (f - n);
    return p;
}

Mat4 lookAtLH(const float eye[3], const float at[3]) {
    // D3DXMatrixLookAtLH with up = +y
    float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z) v /= zl;
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    const float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float& v : x) v /= xl;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat4 m = Mat4::identity();
    for (int i = 0; i < 3; ++i) {
        m.at(i, 0) = x[i];
        m.at(i, 1) = y[i];
        m.at(i, 2) = z[i];
    }
    m.at(3, 0) = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    m.at(3, 1) = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    m.at(3, 2) = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    return m;
}

CameraInput engineCamera(const float eye[3], const float at[3]) {
    CameraInput in;
    in.view = lookAtLH(eye, at);
    in.projection = perspectiveLH(1.0f, 16.0f / 9.0f, 10.0f, 40000.0f);
    in.viewProjection = mul(in.view, in.projection);
    inverse(in.view, in.inverseView);
    for (int i = 0; i < 3; ++i) in.position[static_cast<std::size_t>(i)] = in.inverseView.at(3, i);
    in.engineFar = 40000.0f;
    return in;
}

}  // namespace

TEST(scene_matrix_inverse) {
    const float eye[3] = {1200.0f, 350.0f, -8000.0f}, at[3] = {1500.0f, 300.0f, -7000.0f};
    const CameraInput in = engineCamera(eye, at);
    Mat4 inv;
    CHECK(inverse(in.viewProjection, inv));
    // float matrices with translations of ~8000 units and far/near = 4000: VP * inv is ill-conditioned, so check what the
    // consumers do: world -> clip -> world (depth reconstruction)
    CHECK_MSG(maxAbsDiff(mul(inv, in.viewProjection), Mat4::identity()) < 2e-3f,
              std::to_string(maxAbsDiff(mul(inv, in.viewProjection), Mat4::identity())));
    const float pts[3][3] = {{1500.0f, 300.0f, -7000.0f}, {1300.0f, 360.0f, -7900.0f}, {9000.0f, -200.0f, 20000.0f}};
    for (const auto& p : pts) {
        float clip[4] = {0, 0, 0, 0};
        for (int j = 0; j < 4; ++j) clip[j] = p[0] * in.viewProjection.at(0, j) + p[1] * in.viewProjection.at(1, j) +
                                              p[2] * in.viewProjection.at(2, j) + in.viewProjection.at(3, j);
        float back[4] = {0, 0, 0, 0};
        for (int j = 0; j < 4; ++j) back[j] = clip[0] * inv.at(0, j) + clip[1] * inv.at(1, j) + clip[2] * inv.at(2, j) +
                                              clip[3] * inv.at(3, j);
        const float dist = std::sqrt((p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1]) +
                                     (p[2] - eye[2]) * (p[2] - eye[2]));
        for (int i = 0; i < 3; ++i) CHECK_MSG(std::abs(back[i] / back[3] - p[i]) < 1e-3f * dist + 0.01f, std::to_string(back[i] / back[3]));
    }
    // the inverse view's translation row is the eye
    for (int i = 0; i < 3; ++i) CHECK(std::abs(in.inverseView.at(3, i) - eye[i]) < 0.05f);
    // singular
    Mat4 zero, out;
    CHECK(!inverse(zero, out));
    Mat4 rank3 = Mat4::identity();
    rank3.at(2, 2) = 0.0f;
    CHECK(!inverse(rank3, out));
    CHECK(maxAbsDiff(transpose(transpose(in.view)), in.view) == 0.0f);
}

TEST(scene_near_far) {
    float n = 0, f = 0;
    CHECK(nearFar(perspectiveLH(1.0f, 1.5f, 10.0f, 40000.0f), n, f));
    CHECK(std::abs(n - 10.0f) < 1e-3f);
    // far from a float projection: 1 - f/(f-n) loses digits (~0.1 % at f/n = 4000); engineFar is the exact value
    CHECK_MSG(std::abs(f - 40000.0f) < 40000.0f * 2e-3f, std::to_string(f));
    // right-handed: [2][2] = f/(n-f), [3][2] = n*f/(n-f), [2][3] = -1
    Mat4 rh = perspectiveLH(1.0f, 1.5f, 0.5f, 300.0f);
    rh.at(2, 2) = 300.0f / (0.5f - 300.0f);
    rh.at(3, 2) = 0.5f * 300.0f / (0.5f - 300.0f);
    rh.at(2, 3) = -1.0f;
    CHECK(nearFar(rh, n, f));
    CHECK(std::abs(n - 0.5f) < 1e-4f);
    CHECK(std::abs(f - 300.0f) < 0.05f);
    // orthographic: no near / far from this formula
    CHECK(!nearFar(Mat4::identity(), n, f));
}

TEST(scene_camera_block) {
    const float eye[3] = {100.0f, 200.0f, 300.0f}, at[3] = {150.0f, 190.0f, 400.0f};
    CameraBlock c;
    CHECK(!c.valid);
    c.set(engineCamera(eye, at), 7, 1.5);
    CHECK(c.valid && c.frame == 7 && c.time == 1.5);
    CHECK(!c.prevValid);
    CHECK(std::abs(c.nearZ - 10.0f) < 1e-2f && std::abs(c.farZ - 40000.0f) < 80.0f);
    CHECK(maxAbsDiff(mul(c.viewProjection, c.inverseViewProjection), Mat4::identity()) < 2e-3f);
    c.advance();
    CHECK(!c.valid && c.prevValid);
    CHECK(c.prevPosition[0] == c.position[0]);
    const float eye2[3] = {110.0f, 200.0f, 300.0f};
    c.set(engineCamera(eye2, at), 8, 1.52);
    CHECK(c.prevValid && std::abs(c.position[0] - 110.0f) < 0.05f && std::abs(c.prevPosition[0] - 100.0f) < 0.05f);
    CHECK(maxAbsDiff(c.prevViewProjection, c.viewProjection) > 0.0f);
    // a frame without a main view: no previous camera for the one after it
    c.advance();
    c.advance();
    CHECK(!c.prevValid);
}

TEST(scene_camera_check) {
    const float eye[3] = {-2500.0f, 900.0f, 4100.0f}, at[3] = {-2400.0f, 880.0f, 4200.0f};
    CameraBlock c;
    c.set(engineCamera(eye, at), 1, 0.0);
    CHECK(!checkCamera(c).available);
    // the engine's register layout: HLSL column_major matrices = the transposed matrix in c0..c3, c4..c7; the inverse
    // view's 3x3 in c8..c10; c11 = position + far
    const Mat4 vpT = transpose(c.viewProjection), ipT = transpose(c.inverseProjection), ivT = transpose(c.inverseView);
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) {
            c.vsConstants[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] = vpT.at(r, k);
            c.vsConstants[static_cast<std::size_t>(4 + r)][static_cast<std::size_t>(k)] = ipT.at(r, k);
            if (r < 3) c.vsConstants[static_cast<std::size_t>(8 + r)][static_cast<std::size_t>(k)] = ivT.at(r, k);
        }
    }
    for (std::size_t i = 0; i < 3; ++i) c.vsConstants[11][i] = c.position[i];
    c.vsConstants[11][3] = 40000.0f;
    c.constantsCaptured = true;
    const CameraCheck k = checkCamera(c);
    CHECK(k.available && k.transposed);
    CHECK(k.viewProjection < 1e-6f && k.inverseProjection < 1e-6f && k.position == 0.0f && k.farPlane == 0.0f);
    CHECK_MSG(k.inverseView < 1e-6f, std::to_string(k.inverseView));
    // a wrong camera shows
    c.vsConstants[11][0] += 50.0f;
    c.vsConstants[1][2] += 0.5f;
    const CameraCheck bad = checkCamera(c);
    CHECK(bad.position == 50.0f && bad.viewProjection >= 0.5f);
}

TEST(scene_names) {
    CHECK(std::string(name(View::Main)) == "Main");
    CHECK(std::string(name(Pass::Refracted)) == "Refracted");
    CHECK(std::string(name(ImageRole::DepthMap)) == "depth map");
    for (int v = 0; v < static_cast<int>(View::Count); ++v) CHECK(std::string(name(static_cast<View>(v))) != "?");
    for (int p = 0; p < static_cast<int>(Pass::Count); ++p) CHECK(std::string(name(static_cast<Pass>(p))) != "?");
    for (int r = 0; r < static_cast<int>(ImageRole::Count); ++r) CHECK(std::string(name(static_cast<ImageRole>(r))) != "?");
}
