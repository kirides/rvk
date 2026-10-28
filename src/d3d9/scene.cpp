#include "d3d9/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "shader/abi.h"

namespace rvk::d3d9::scene {

const char* name(View v) {
    switch (v) {
        case View::None: return "None";
        case View::Main: return "Main";
        case View::Shadow: return "Shadow";
        case View::SkyCube: return "SkyCube";
        case View::Preview: return "Preview";
        case View::Post: return "Post";
        case View::Gui: return "Gui";
        case View::Other: return "Other";
        case View::Count: break;
    }
    return "?";
}

const char* name(Pass p) {
    switch (p) {
        case Pass::None: return "None";
        case Pass::Setup: return "Setup";
        case Pass::DepthPrepass: return "DepthPrepass";
        case Pass::ShadowMask: return "ShadowMask";
        case Pass::ShadowDepth: return "ShadowDepth";
        case Pass::Solid: return "Solid";
        case Pass::Refracted: return "Refracted";
        case Pass::Alpha: return "Alpha";
        case Pass::AlphaLate: return "AlphaLate";
        case Pass::Sky: return "Sky";
        case Pass::Post: return "Post";
        case Pass::Gui: return "Gui";
        case Pass::Count: break;
    }
    return "?";
}

const char* name(ImageRole r) {
    switch (r) {
        case ImageRole::None: return "none";
        case ImageRole::SceneHdr: return "scene HDR target";
        case ImageRole::Refraction: return "refraction copy";
        case ImageRole::DepthMap: return "depth map";
        case ImageRole::SkyEnvCube: return "sky environment cube";
        case ImageRole::ShadowMap: return "shadow map";
        case ImageRole::ShadowCube: return "shadow cube";
        case ImageRole::Count: break;
    }
    return "?";
}

// ---- phases ----------------------------------------------------------------------------------------------------------------

void PhaseTracker::open(bool enter) {
    if (!enter && !segments_.empty()) {
        Segment& last = segments_.back();
        // nothing drawn in the previous stretch and nothing else to remember: reuse it
        if (last.draws == 0 && !last.enter && last.depth == stack_.size()) {
            last.phase = cur_;
            return;
        }
    }
    if (segments_.size() >= kMaxSegments) {
        overflow_ = true;
        return;
    }
    segments_.push_back(Segment{cur_, static_cast<std::uint8_t>(std::min<std::size_t>(stack_.size(), 255)), enter, false, 0});
}

void PhaseTracker::push(View view, Pass pass) {
    if (stack_.size() >= kMaxDepth) {
        ++unbalanced_;
        return;
    }
    stack_.push_back(cur_);
    cur_ = Phase{view, pass};
    ++generation_;
    ++entered_[static_cast<std::size_t>(view)];
    open(true);
}

void PhaseTracker::pop() {
    if (stack_.empty()) {
        ++unbalanced_;
        return;
    }
    cur_ = stack_.back();
    stack_.pop_back();
    ++generation_;
    open(false);
}

void PhaseTracker::setPass(Pass pass) {
    if (cur_.pass == pass) return;
    cur_.pass = pass;
    ++generation_;
    if (!segments_.empty() && segments_.back().enter && segments_.back().draws == 0 && segments_.back().depth == stack_.size()) {
        segments_.back().phase = cur_;  // a view entered and its pass changed before any draw
        return;
    }
    open(false);
}

void PhaseTracker::countDraw() {
    ++draws_;
    if (segments_.empty()) open(false);  // draws before any marker of the frame
    if (!overflow_ && !segments_.empty()) ++segments_.back().draws;
}

void PhaseTracker::endFrame() {
    ++generation_;
    last_.swap(segments_);
    segments_.clear();
    lastEntered_ = entered_;
    entered_.fill(0);
    lastDraws_ = draws_;
    draws_ = 0;
    overflow_ = false;
    // the open scopes continue into the next frame: re-open them so the next sequence shows where it starts
    for (std::size_t i = 0; i < stack_.size(); ++i) {
        const Phase p = i + 1 < stack_.size() ? stack_[i + 1] : cur_;
        segments_.push_back(Segment{p, static_cast<std::uint8_t>(i + 1), true, true, 0});
    }
}

std::string PhaseTracker::summary(const std::vector<Segment>& segments) {
    std::string out;
    struct Open {
        std::size_t start;  // position of "View{" in out
        bool continued;     // a scope open at the frame start (left out when nothing happens in it)
        bool used;
    };
    std::vector<Open> scopes;
    Pass lastPass = Pass::None;  // the last pass token at the current depth (merging)
    std::size_t lastPassPos = std::string::npos;
    std::uint32_t lastPassDraws = 0;
    const auto sep = [&] {
        if (!out.empty() && out.back() != '{') out += ' ';
    };
    const auto close = [&](std::size_t to) {
        while (scopes.size() > to) {
            const Open o = scopes.back();
            scopes.pop_back();
            if (o.continued && !o.used) {
                out.resize(o.start);
                while (!out.empty() && out.back() == ' ') out.pop_back();
            } else {
                out += '}';
                if (!scopes.empty()) scopes.back().used = true;
            }
            lastPass = Pass::None;
            lastPassPos = std::string::npos;
        }
    };
    for (const Segment& s : segments) {
        if (s.enter) {
            close(s.depth > 0 ? s.depth - 1u : 0u);
            if (!scopes.empty()) scopes.back().used = true;
            sep();
            scopes.push_back(Open{out.size(), s.continued, false});
            out += std::string(name(s.phase.view)) + "{";
            lastPass = Pass::None;
            lastPassPos = std::string::npos;
        } else if (s.depth < scopes.size()) {
            close(s.depth);
        }
        if (s.draws == 0) continue;
        if (!scopes.empty()) scopes.back().used = true;
        if (s.phase.pass == lastPass && lastPassPos != std::string::npos) {
            lastPassDraws += s.draws;
            out.resize(lastPassPos);
            out += std::format("{}({})", name(s.phase.pass), lastPassDraws);
            continue;
        }
        sep();
        lastPassPos = out.size();
        lastPass = s.phase.pass;
        lastPassDraws = s.draws;
        out += std::format("{}({})", name(s.phase.pass), s.draws);
    }
    close(0);
    return out;
}

// ---- camera ----------------------------------------------------------------------------------------------------------------

Mat4 Mat4::identity() {
    Mat4 r;
    for (int i = 0; i < 4; ++i) r.at(i, i) = 1.0f;
    return r;
}

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += static_cast<double>(a.at(i, k)) * b.at(k, j);
            r.at(i, j) = static_cast<float>(s);
        }
    }
    return r;
}

Mat4 transpose(const Mat4& a) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.at(i, j) = a.at(j, i);
    return r;
}

bool inverse(const Mat4& a, Mat4& out) {
    // Gauss-Jordan with partial pivoting in double precision
    double m[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i][j] = a.at(i, j);
            m[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    double scale = 0.0;
    for (const float v : a.m) scale = std::max(scale, std::abs(static_cast<double>(v)));
    if (scale == 0.0 || !std::isfinite(scale)) return false;
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::abs(m[r][c]) > std::abs(m[pivot][c])) pivot = r;
        }
        if (std::abs(m[pivot][c]) <= 1e-12 * scale) return false;
        if (pivot != c) {
            for (int j = 0; j < 8; ++j) std::swap(m[c][j], m[pivot][j]);
        }
        const double inv = 1.0 / m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c || m[r][c] == 0.0) continue;
            const double f = m[r][c];
            for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out.at(i, j) = static_cast<float>(m[i][j + 4]);
    return true;
}

float maxAbsDiff(const Mat4& a, const Mat4& b) {
    float d = 0.0f;
    for (std::size_t i = 0; i < 16; ++i) d = std::max(d, std::abs(a.m[i] - b.m[i]));
    return d;
}

bool nearFar(const Mat4& p, float& nearZ, float& farZ) {
    // D3DXMatrixPerspective*LH: [2][2] = f/(f-n), [3][2] = -n*f/(f-n), [2][3] = 1, [3][3] = 0; RH: [2][2] = f/(n-f),
    // [3][2] = n*f/(n-f), [2][3] = -1.
    const double a = p.at(2, 2), b = p.at(3, 2), w = p.at(2, 3);
    if (p.at(3, 3) != 0.0f || (w != 1.0 && w != -1.0) || a == 0.0) return false;
    // z_ndc = (a*z + b) / (w*z): z_ndc = 0 at z = n, 1 at z = f (z = view-space depth along the w sign)
    const double n = -b / a;
    const double f = b / (w - a);
    if (!std::isfinite(n) || !std::isfinite(f)) return false;
    nearZ = static_cast<float>(w > 0 ? n : -n);
    farZ = static_cast<float>(w > 0 ? f : -f);
    return nearZ > 0.0f && farZ > nearZ;
}

void CameraBlock::set(const CameraInput& in, std::uint64_t frameIndex, double seconds) {
    valid = true;
    frame = frameIndex;
    time = seconds;
    view = in.view;
    projection = in.projection;
    viewProjection = in.viewProjection;
    inverseView = in.inverseView;
    position = in.position;
    engineFar = in.engineFar;
    if (!inverse(viewProjection, inverseViewProjection)) inverseViewProjection = Mat4::identity();
    if (!inverse(projection, inverseProjection)) inverseProjection = Mat4::identity();
    if (!nearFar(projection, nearZ, farZ)) {
        nearZ = 0.0f;
        farZ = 0.0f;
    }
}

void CameraBlock::advance() {
    prevValid = valid;
    if (valid) {
        prevViewProjection = viewProjection;
        prevInverseViewProjection = inverseViewProjection;
        prevPosition = position;
    }
    valid = false;
    constantsCaptured = false;
}

CameraCheck checkCamera(const CameraBlock& cam) {
    CameraCheck c;
    if (!cam.valid || !cam.constantsCaptured) return c;
    c.available = true;
    const auto regs = [&](int first, int count) {
        Mat4 r;
        for (int i = 0; i < count; ++i)
            for (int j = 0; j < 4; ++j) r.at(i, j) = cam.vsConstants[static_cast<std::size_t>(first + i)][static_cast<std::size_t>(j)];
        return r;
    };
    const auto rows3 = [](const Mat4& a) {  // only the first three rows count (float3x3 / 4x3 registers)
        Mat4 r = a;
        for (int j = 0; j < 4; ++j) r.at(3, j) = 0.0f;
        return r;
    };
    const Mat4 c0 = regs(0, 4), c4 = regs(4, 4), c8 = rows3(regs(8, 3));
    const float asRows = maxAbsDiff(c0, cam.viewProjection);
    const float asCols = maxAbsDiff(c0, transpose(cam.viewProjection));
    c.transposed = asCols < asRows;
    c.viewProjection = std::min(asRows, asCols);
    const Mat4& ip = cam.inverseProjection;
    c.inverseProjection = c.transposed ? maxAbsDiff(c4, transpose(ip)) : maxAbsDiff(c4, ip);
    // c8..c10: the inverse view's 3x3 part; compare both layouts (the engine writes it with another helper than c0)
    Mat4 iv = cam.inverseView, ivt = transpose(cam.inverseView);
    for (int i = 0; i < 4; ++i) {
        iv.at(i, 3) = c8.at(i, 3);  // the 4th column of c8..c10 is not part of the 3x3 matrix
        ivt.at(i, 3) = c8.at(i, 3);
    }
    c.inverseView = std::min(maxAbsDiff(c8, rows3(iv)), maxAbsDiff(c8, rows3(ivt)));
    float dp = 0.0f;
    for (std::size_t i = 0; i < 3; ++i) dp = std::max(dp, std::abs(cam.vsConstants[11][i] - cam.position[i]));
    c.position = dp;
    c.farPlane = std::abs(cam.vsConstants[11][3] - cam.engineFar);
    for (const float v : cam.viewProjection.m) c.scale = std::max(c.scale, std::abs(v));
    return c;
}

// ---- frame block ---------------------------------------------------------------------------------------------------------

void fillFrameBlock(shader::abi::FrameBlock& out, const CameraBlock& cur, const CameraBlock& last, const FrameExtras& x) {
    namespace abi = shader::abi;
    out = abi::FrameBlock{};
    const auto put = [](float (&dst)[16], const Mat4& m) { std::memcpy(dst, m.m.data(), sizeof(dst)); };
    const CameraBlock* cam = cur.valid ? &cur : last.valid ? &last : nullptr;
    std::uint32_t flags = cur.valid ? abi::kFrameCameraValid : last.valid ? abi::kFrameCameraPredicted : 0u;
    if (x.reverseZ) flags |= abi::kFrameReverseZ;
    if (cam != nullptr) {
        put(out.viewProj, cam->viewProjection);
        put(out.invViewProj, cam->inverseViewProjection);
        put(out.view, cam->view);
        put(out.proj, cam->projection);
        put(out.invView, cam->inverseView);
        put(out.invProj, cam->inverseProjection);
        out.cameraPos[0] = cam->position[0];
        out.cameraPos[1] = cam->position[1];
        out.cameraPos[2] = cam->position[2];
        out.cameraPos[3] = cam->engineFar;
        out.depthParams[0] = cam->nearZ;
        out.depthParams[1] = cam->farZ;
        out.depthParams[2] = cam->engineFar;
        out.depthParams[3] = cam->engineFar > 0.0f ? 1.0f / cam->engineFar : 0.0f;
        if (cam->prevValid) {
            flags |= abi::kFramePrevValid;
            put(out.prevViewProj, cam->prevViewProjection);
            put(out.prevInvViewProj, cam->prevInverseViewProjection);
            out.prevCameraPos[0] = cam->prevPosition[0];
            out.prevCameraPos[1] = cam->prevPosition[1];
            out.prevCameraPos[2] = cam->prevPosition[2];
        } else {
            std::memcpy(out.prevViewProj, out.viewProj, sizeof(out.viewProj));
            std::memcpy(out.prevInvViewProj, out.invViewProj, sizeof(out.invViewProj));
            std::memcpy(out.prevCameraPos, out.cameraPos, sizeof(float) * 3);
        }
    } else {
        const Mat4 id = Mat4::identity();
        for (float (*m)[16] : {&out.viewProj, &out.invViewProj, &out.view, &out.proj, &out.invView, &out.invProj, &out.prevViewProj,
                               &out.prevInvViewProj}) {
            put(*m, id);
        }
    }
    out.viewport[0] = x.width;
    out.viewport[1] = x.height;
    out.viewport[2] = x.width > 0.0f ? 1.0f / x.width : 0.0f;
    out.viewport[3] = x.height > 0.0f ? 1.0f / x.height : 0.0f;
    out.time[0] = static_cast<float>(cur.valid ? cur.time : x.now);
    out.time[1] = cur.valid && last.valid ? static_cast<float>(cur.time - last.time) : 0.0f;
    for (std::size_t i = 0; i < 4; ++i) out.jitter[i] = x.jitter[i];
    out.frameInfo[0] = static_cast<std::uint32_t>(x.frameIndex);
    out.frameInfo[1] = flags;
    out.frameInfo[2] = x.mainViews;
    for (std::size_t i = 0; i < 4; ++i) out.images[i] = x.images[i];
    if (x.params != nullptr) {
        for (std::size_t i = 0; i < abi::kFrameParams; ++i) std::memcpy(out.params[i], (*x.params)[i].data(), sizeof(out.params[i]));
    }
    if (x.rtTlas != 0) {
        out.rtScene[0] = static_cast<std::uint32_t>(x.rtTlas);
        out.rtScene[1] = static_cast<std::uint32_t>(x.rtTlas >> 32);
        out.rtScene[2] = static_cast<std::uint32_t>(x.rtRecords);
        out.rtScene[3] = static_cast<std::uint32_t>(x.rtRecords >> 32);
        out.rtInfo[0] = x.rtInstances;
        out.rtInfo[1] = x.rtRecordCount;
        out.rtInfo[2] = abi::kFrameRtValid;
    }
    if (x.gi != 0) {
        out.gi[0] = static_cast<std::uint32_t>(x.gi);
        out.gi[1] = static_cast<std::uint32_t>(x.gi >> 32);
        out.gi[2] = abi::kFrameGiValid;
    }
    if (x.ao[0] != 0) {
        for (std::size_t i = 0; i < 4; ++i) out.ao[i] = x.ao[i];
    }
    if (x.sgi[0] != 0) {
        for (std::size_t i = 0; i < 4; ++i) out.sgi[i] = x.sgi[i];
    }
}

}  // namespace rvk::d3d9::scene
