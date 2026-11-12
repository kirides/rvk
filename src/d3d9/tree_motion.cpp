// Tree / grass wind motion (native.tree_motion, docs/research/frame-ownership.md 14.42). Per-object motion of the swaying SpeedTrees (branches, fronds, leaf cards, leaf
// meshes) and the grass without vertex captures: the wind-motion vertex shader forms (VariantSlot WindVs = the push form of composed draws / packet replays, SceneVsWind2 = the
// scene ABI v2 form of the owned tree / grass flushes) run the translated program a second time with VS window 1 (c12..c30: the wind matrices c14..c22, the leaf angle matrix
// c23..c25, the rock / rustle angles c26..c29, the vegetation clock / wind c12) pointed at the PREVIOUS frame's block (RVK_WIND_PREV, rvk_draw.hlsli) and output the previous
// world position (rvk_prevWorld, location 15) that the G-buffer motion pixel variants (GbufferMotion / -M and their v2 scene forms) turn into the motion vector, as they do for
// the bone-motion forms (14.13). The first run is the translated program unchanged: the clip position stays bit-identical to every other form of the shader (the z-equal colour
// pass, shader-permutations.md 12). With debug.ubershader the v2 form is the port's wind form (RVK_UBER_WIND_PREV, ge_default.vert.hlsl: Transform run on both windows, the
// displacement added to the translated clip position's world point).
//
// The previous block: the wind constants are global per frame (eCSpeedTreeAdmin / eCVegetationAdmin; the owned trees put them in one view block per pass), so the device keeps
// per kind (trees, grass) the c12..c30 of this frame's main solid draws and of the last frame (keyed by the memory epoch: 14.40) and hands each draw of the frame one ring block
// of the last frame's (prevVs: the push block's / the DrawRecord's). A frame whose draws disagree on the wind registers (the kind's registers only: trees c14..c29, grass c12),
// a gap of a frame, or a still wind = no block: the plain forms (the camera's motion).
//
// Compare (native.tree_motion=compare at start, Vulkan): the translated wind form of a SpeedTree shader also evaluates F4-5's wind math (rvk_tree_wind.hlsli, the compute pass's
// function) with the previous frame's constants and the instance's rows and compares it with its rerun's previous position (pixels on this frame's screen through c0..c3, world
// units through the reconstruction), plus the same evaluation with THIS frame's constants against the first run (the noise floor); accumulated into a Readback buffer
// (frame param kFrameParamTreeMotion).

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "shader/abi.h"

namespace rvk::d3d9 {

namespace {

constexpr std::uint32_t kPrevWorldLocation = 15;  // enh::kPrevWorldLocation (the motion pixel variants' varying)
constexpr std::uint32_t kCmpSlots = rhi::kFramesInFlight + 2;
constexpr std::uint32_t kCmpBytes = 32;  // per slot: max px bits, compared, max world bits, over 0.01 px, floor px bits, floor world bits, still, -

std::atomic<bool> g_compare{false};
bool g_compareFixed = false;  // (the compare text is decided by the first mode set: the variants are built once)

// the second run of the translated program on the previous frame's window 1 (prevVs: the block's biased base, 0 = none)
const char* kWindEpilogue =
    "    rvk_out.rvk_prevWorld = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    {\n"
    "        // tree / grass wind motion (native.tree_motion, frame-ownership.md 14.42): the previous frame's c12..c30 (prevVs, 0 = none)\n"
    "        const uint2 rvk_wmRef = rvkDrawPrevVs();\n"
    "        const bool rvk_wmHas = any(rvk_wmRef != uint2(0u, 0u));\n"
    "        const uint2 rvk_wmPrev = rvk_wmHas ? rvk_wmRef : rvkVsWin(1u);  // (never the zero pair: a load hoisted above the test stays in a live buffer)\n"
    "        if (rvk_wmHas) {\n"
    "            rvk_vsw1Ov = rvk_wmPrev;\n"
    "            rvk_resetRegs();\n"
    "            rvk_main();\n"
    "            rvk_vsw1Ov = uint2(0u, 0u);\n"
    "            // (precise like the first run's position: the same operations in the same order; back to the world through the inverse view-projection)\n"
    "            precise float4 rvk_wmP = $pos;\n"
    "            const float4 rvk_wmW = mul(rvk_wmP, rvkLoadFrame(pc.frame).invViewProj);\n"
    "            // (a vertex the wind does not move (the same clip position, bit for bit) keeps w = 0: the camera's motion from the motion shader's own reconstruction)\n"
    "            if (abs(rvk_wmW.w) > 1e-20 && any(rvk_wmP != rvk_pos)) rvk_out.rvk_prevWorld = float4(rvk_wmW.xyz / rvk_wmW.w, 1.0);\n"
    "$cmp"
    "        }\n"
    "    }";

// the compare block (inside `if (rvk_wmHas)`): the shader's inputs by usage (v<register>: the translator's input globals, unchanged by the reruns)
std::string compareText(const Shader& s) {
    int pos = -1, idx = -1, w0 = -1, w1 = -1, w2 = -1, n1 = -1, n2 = -1, inst = -1;
    for (const shader::Reflection::VertexInput& v : s.reflection.vertexInputs) {
        const int r = static_cast<int>(v.location);
        if (v.usage == shader::Usage::Position && v.usageIndex == 0) pos = r;
        else if (v.usage == shader::Usage::BlendIndices && v.usageIndex == 0) idx = r;
        else if (v.usage == shader::Usage::BlendWeight && v.usageIndex == 0) w0 = r;
        else if (v.usage == shader::Usage::BlendWeight && v.usageIndex == 1) w1 = r;
        else if (v.usage == shader::Usage::BlendWeight && v.usageIndex == 2) w2 = r;
        else if (v.usage == shader::Usage::Normal && v.usageIndex == 1) n1 = r;
        else if (v.usage == shader::Usage::Normal && v.usageIndex == 2) n2 = r;
        else if (v.usage == shader::Usage::TexCoord && v.usageIndex == 4) inst = r;
    }
    if (pos < 0 || idx < 0 || w0 < 0) return {};  // (not a SpeedTree shader: the grass' wind is another formula)
    // (the class as treewind::layoutOf makes it: a leaf mesh declares NORMAL1 / 2 + BLENDWEIGHT2, leaf cards BLENDWEIGHT1 + 2, the rest is wind only)
    const unsigned cls = (w2 >= 0 && n1 >= 0 && n2 >= 0) ? 1u : (w2 >= 0 && w1 >= 0) ? 2u : 0u;
    const auto in = [](int r) { return r >= 0 ? std::format("v{}", r) : std::string("float4(0.0, 0.0, 0.0, 1.0)"); };
    return std::format(
        "            {{\n"
        "                // native.tree_motion=compare: F4-5's wind math (rvk_tree_wind.hlsli) with the previous frame's constants against the rerun (+ this frame's against the first run)\n"
        "                const RvkFrame rvk_tcF = rvkLoadFrame(pc.frame);\n"
        "                const uint2 rvk_tcBuf = uint2(asuint(rvk_tcF.params[{0}].x), asuint(rvk_tcF.params[{0}].y));\n"
        "                if (any(rvk_tcBuf != uint2(0u, 0u)) && rvk_tcF.params[{0}].z == 1.0) {{\n"
        "                    const RvkAddr rvk_tcE = rvkAddr(rvk_tcBuf);\n"
        "                    RvkTreeWindConsts rvk_tcP, rvk_tcC;\n"
        "                    const int rvk_tcI = int({1}.x);\n"
        "                    [unroll] for (int rvk_k = 0; rvk_k < 4; ++rvk_k) {{\n"
        "                        rvk_tcC.k[16 + rvk_k] = RVK_VSC(32 + 4 * rvk_tcI + rvk_k);\n"
        "                        rvk_tcP.k[16 + rvk_k] = rvk_tcC.k[16 + rvk_k];\n"
        "                    }}\n"
        "                    [unroll] for (int rvk_k = 0; rvk_k < 16; ++rvk_k) rvk_tcC.k[rvk_k] = RVK_VSC(14 + rvk_k);\n"
        "                    rvk_vsw1Ov = rvk_wmPrev;\n"
        "                    [unroll] for (int rvk_k = 0; rvk_k < 16; ++rvk_k) rvk_tcP.k[rvk_k] = RVK_VSC(14 + rvk_k);\n"
        "                    rvk_vsw1Ov = uint2(0u, 0u);\n"
        "                    const float3 rvk_tcWp = rvkTreeWindWorld(rvk_tcP, {2}, int({3}.x), {4}, {5}, {6}, {7}.xyz, {8}.xyz, {9}u);\n"
        "                    const float3 rvk_tcWc = rvkTreeWindWorld(rvk_tcC, {2}, int({3}.x), {4}, {5}, {6}, {7}.xyz, {8}.xyz, {9}u);\n"
        "                    // (pixels: both previous positions through this frame's view-projection c0..c3, as the first run projects)\n"
        "                    const float4 rvk_tcVp = float4(dot(float4(rvk_tcWp, 1.0), RVK_VSC(0)), dot(float4(rvk_tcWp, 1.0), RVK_VSC(1)), dot(float4(rvk_tcWp, 1.0), RVK_VSC(2)), dot(float4(rvk_tcWp, 1.0), RVK_VSC(3)));\n"
        "                    const float4 rvk_tcVc = float4(dot(float4(rvk_tcWc, 1.0), RVK_VSC(0)), dot(float4(rvk_tcWc, 1.0), RVK_VSC(1)), dot(float4(rvk_tcWc, 1.0), RVK_VSC(2)), dot(float4(rvk_tcWc, 1.0), RVK_VSC(3)));\n"
        "                    // (on-screen vertices in front of the camera: w > 1 unit (the near-plane vertices' divisions blow any difference up), |ndc| <= 1)\n"
        "                    if (rvk_tcVp.w > 1.0 && rvk_wmP.w > 1.0 && rvk_tcVc.w > 1.0 && rvk_pos.w > 1.0 && abs(rvk_wmW.w) > 1e-20 && all(abs(rvk_pos.xy) <= rvk_pos.w)) {{\n"
        "                        const float2 rvk_tcVpx = rvk_tcF.viewport.xy * 0.5;\n"
        "                        const float rvk_tcPx = length((rvk_tcVp.xy / rvk_tcVp.w - rvk_wmP.xy / rvk_wmP.w) * rvk_tcVpx);\n"
        "                        const float rvk_tcFx = length((rvk_tcVc.xy / rvk_tcVc.w - rvk_pos.xy / rvk_pos.w) * rvk_tcVpx);\n"
        "                        const float4 rvk_tcW1 = mul(rvk_pos, rvk_tcF.invViewProj);\n"
        "                        rvkRwAtomicMax(rvk_tcE, asuint(rvk_tcPx));\n"
        "                        rvkRwAtomicAdd(rvkAddrAdd(rvk_tcE, 4u), 1u);\n"
        "                        rvkRwAtomicMax(rvkAddrAdd(rvk_tcE, 8u), asuint(length(rvk_wmW.xyz / rvk_wmW.w - rvk_tcWp)));\n"
        "                        if (rvk_tcPx > 0.01) rvkRwAtomicAdd(rvkAddrAdd(rvk_tcE, 12u), 1u);\n"
        "                        rvkRwAtomicMax(rvkAddrAdd(rvk_tcE, 16u), asuint(rvk_tcFx));\n"
        "                        if (rvk_tcFx > 0.01) rvkRwAtomicAdd(rvkAddrAdd(rvk_tcE, 28u), 1u);\n"
        "                        if (abs(rvk_tcW1.w) > 1e-20) rvkRwAtomicMax(rvkAddrAdd(rvk_tcE, 20u), asuint(length(rvk_tcW1.xyz / rvk_tcW1.w - rvk_tcWc)));\n"
        "                        if (all(rvk_wmP == rvk_pos)) rvkRwAtomicAdd(rvkAddrAdd(rvk_tcE, 24u), 1u);\n"
        "                    }}\n"
        "                }}\n"
        "            }}\n",
        shader::abi::kFrameParamTreeMotion, inst >= 0 ? std::format("v{}", inst) : std::string("float4(0.0, 0.0, 0.0, 0.0)"), in(pos), in(idx), in(w0), in(w1), in(w2), in(n1), in(n2), cls);
}

}  // namespace

bool Device::windVsOptions(const Shader& s, shader::TranslateOptions& o) {
    if (s.stage != shader::Stage::Vertex) return false;
    o.defines += "#define RVK_WIND_PREV 1\n";
    o.rerunnable = true;
    o.extraOutputs += std::format("    [[vk::location({})]] float4 rvk_prevWorld : VARY{};", kPrevWorldLocation, kPrevWorldLocation);
    std::string ep = kWindEpilogue;
    std::string cmp;
    if (g_compare.load(std::memory_order_relaxed)) {
        cmp = compareText(s);
        if (!cmp.empty()) o.prelude += "#include \"rvk_tree_wind.hlsli\"\n";
    }
    ep.replace(ep.find("$cmp"), 4, cmp);
    o.epilogue += ep;
    return true;
}

bool Device::windCompareOn() { return g_compare.load(std::memory_order_relaxed); }
void Device::windCompareForce(bool on) { g_compare.store(on, std::memory_order_relaxed); }

void Device::setTreeMotion(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    if (!g_compareFixed && mode != 0) {
        // (the compare text is part of the variants, which are built once: the mode of the first switch-on decides it for the session)
        g_compareFixed = true;
        g_compare.store(mode == 2, std::memory_order_relaxed);
        setVariantProvider(VariantSlot::WindVs, {mode == 2 ? "wmot2c" : "wmot2", windVsOptions});  // (bump the tags when kWindEpilogue / compareText change: the SPIR-V cache keys on the tag, not the text)
    }
    if (mode == 2 && !g_compare.load(std::memory_order_relaxed)) {
        spdlog::warn("native: tree motion: compare needs a restart (the wind forms were built without the compare text): on");
        mode = 1;
    } else if (mode == 1 && g_compare.load(std::memory_order_relaxed)) {
        mode = 2;  // (the forms carry the compare text: it stays on with them)
    }
    if (mode == windMode_) return;
    windMode_ = mode;
    ++variantEpoch_;
    for (WindHist& h : windHist_) h = WindHist{};
    spdlog::info("native: tree motion {}", mode == 0 ? "off" : mode == 1 ? "on (the swaying trees' / grass' motion from the previous frame's wind constants)" : "compare (on + a GPU compare with F4-5's wind math)");
}

void Device::windNote(int kind, const float (*c12)[4]) {
    if (kind < 0 || kind > 1 || c12 == nullptr || game_ == nullptr) return;
    WindHist& h = windHist_[kind];
    // (the present count, not the memory epoch: a submit inside the frame may start a new epoch, and "last frame" must stay the last PRESENTED frame:
    // rolled at such a submit the history held this frame's constants and the second run gave this frame's position, no motion)
    const std::uint64_t epoch = stats_.frames + 1;
    ++windStats.notes;
    if (h.epoch != epoch) {
        h.prevValid = h.epoch != 0 && h.epoch + 1 == epoch && !h.conflict;
        if (h.prevValid) std::memcpy(h.prev, h.cur, sizeof(h.prev));
        std::memcpy(h.cur, c12, sizeof(h.cur));
        h.conflict = false;
        h.epoch = epoch;
        h.block = {};
        h.blockEpoch = 0;
        if (h.prevValid && game_->memoryEpoch() != h.noteEpoch + 1 && h.noteEpoch != 0) ++windStats.epochGaps;  // (memory epochs that were not one per present)
        h.noteEpoch = game_->memoryEpoch();
        return;
    }
    // (the registers of the kind's wind only: the composition writes those; the rest of the window is another draw's in lean mode)
    const std::size_t first = kind == 0 ? 2 : 0, count = kind == 0 ? 16 : 1;
    if (!h.conflict && std::memcmp(h.cur[first], c12[first], count * 16) != 0) {
        h.conflict = true;
        ++windStats.conflicts;
    }
}

rhi::GpuRef Device::windPrev(int kind) {
    if (kind < 0 || kind > 1 || game_ == nullptr || windMode_ == 0) return {};
    WindHist& h = windHist_[kind];
    const std::uint64_t epoch = stats_.frames + 1;
    if (h.epoch != epoch || !h.prevValid || h.conflict) {
        ++windStats.prevNone;
        return {};
    }
    // (the ring block lives for the memory epoch: a new epoch inside the frame gets a new block of the same history)
    const std::uint64_t memEpoch = game_->memoryEpoch();
    if (h.blockEpoch == memEpoch) {
        if (!h.block.null()) ++windStats.prevValid;
        else ++windStats.prevStill;
        return h.block;
    }
    h.blockEpoch = memEpoch;
    const std::size_t first = kind == 0 ? 2 : 0, count = kind == 0 ? 16 : 1;
    if (std::memcmp(h.prev[first], h.cur[first], count * 16) == 0) {
        h.block = {};  // (a still wind: no motion of its own)
        ++windStats.prevStill;
        return {};
    }
    // c12..c30 of the last frame (19 float4) + c31 (zero: never read through window 1)
    rhi::RingAlloc a = game_->allocRing(20 * 16, 16);
    if (a.ptr == nullptr || a.ref.null()) {
        h.block = {};
        ++windStats.prevNone;
        return {};
    }
    std::memcpy(a.ptr, h.prev, sizeof(h.prev));
    std::memset(static_cast<std::uint8_t*>(a.ptr) + sizeof(h.prev), 0, 16);
    h.block = a.ref.plus(-static_cast<std::int64_t>(shader::abi::kVsWindowFirst[1]) * 16);  // (biased like every window block: c[n] at base + n x 16)
    ++windStats.blocks;
    ++windStats.prevValid;
    return h.block;
}

bool Device::windStale(const CachedDraw& c, bool v2) const {
    if (windMode_ == 0 || windKindOf(c.kind) < 0 || c.phase.view != scene::View::Main || c.phase.pass != scene::Pass::Solid || !c.v.gbufWanted || gbuf_.motion == nullptr ||
        c.vs == nullptr || c.ps == nullptr) {
        return false;
    }
    if ((v2 ? c.windPipeline2 : c.windPipeline) != GamePipeline::None) return false;
    const auto built = [](const Shader& s, VariantSlot v) { return s.variantState[static_cast<std::size_t>(v)].load(std::memory_order_acquire) == 2; };
    const VariantSlot fs = gbufferSlot(true);
    if (v2) {
        const VariantSlot fs2 = sceneView2Of(sceneSlotOf(fs));
        return built(*c.vs, VariantSlot::SceneVsWind2) && built(*c.ps, fs2);
    }
    return built(*c.vs, VariantSlot::WindVs) && built(*c.ps, fs);
}

void Device::treeMotionFrame() {
    if (windMode_ == 0) return;
    TreeMotionStats& s = windStats;
    if (windMode_ == 2 && options_.backend == rhi::Backend::Vulkan) {
        if (windCmp_ == nullptr && rhi() != nullptr) {
            windCmp_ = rhi()->createBuffer({static_cast<std::uint64_t>(kCmpBytes) * kCmpSlots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Readback, "tree motion compare"});
            if (windCmp_ != nullptr && windCmp_->map() != nullptr) std::memset(windCmp_->map(), 0, static_cast<std::size_t>(kCmpBytes) * kCmpSlots);
        }
        if (windCmp_ != nullptr && windCmp_->map() != nullptr) {
            // the slot of the next frame: its last use was kCmpSlots frames ago (> the frames in flight: the GPU is done with it)
            const std::uint32_t slot = static_cast<std::uint32_t>(++windCmpFrame_ % kCmpSlots);
            windCmp_->invalidate();
            auto* e = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(windCmp_->map()) + static_cast<std::size_t>(kCmpBytes) * slot);
            const float px = std::bit_cast<float>(e[0]), world = std::bit_cast<float>(e[2]), fpx = std::bit_cast<float>(e[4]), fworld = std::bit_cast<float>(e[5]);
            s.compared += e[1];
            s.compareOver += e[3];
            s.compareStill += e[6];
            s.compareFloorOver += e[7];
            s.winPx = std::max(s.winPx, px);
            s.winWorld = std::max(s.winWorld, world);
            s.winFloorPx = std::max(s.winFloorPx, fpx);
            s.winFloorWorld = std::max(s.winFloorWorld, fworld);
            s.maxPx = std::max(s.maxPx, px);
            s.maxWorld = std::max(s.maxWorld, world);
            std::memset(e, 0, kCmpBytes);
            windCmp_->flush();
            const std::array<std::uint32_t, 2> ref = windCmp_->shaderRef(static_cast<std::uint64_t>(kCmpBytes) * slot);
            setFrameParam(shader::abi::kFrameParamTreeMotion, {std::bit_cast<float>(ref[0]), std::bit_cast<float>(ref[1]), 1.0f, 0.0f});
        }
    }
    const double now = secondsSinceCreate();
    if (now - windLog_ < 5.0) return;
    windLog_ = now;
    const TreeMotionStats& p = windLast_;
    spdlog::info("native: tree motion (5 s): drawn with the wind-motion forms: composed {} (pending {}), replays {} (made again for the forms {}), owned items {} ({} instances; v2 forms "
                 "chosen {}, pending {}, items without a form {}); trees {} / grass {}; previous wind block: {} given, {} new blocks, none {} (no / ambiguous history), still {}; "
                 "wind registers noted {}, frames whose draws disagreed {}, frames with more than one memory epoch {}",
                 s.pushDrawn - p.pushDrawn, s.pushPending - p.pushPending, s.replays - p.replays, s.staleRefused - p.staleRefused, s.ownedItems - p.ownedItems,
                 s.ownedInstances - p.ownedInstances, s.v2Chosen - p.v2Chosen, s.v2Pending - p.v2Pending, s.ownedPlain - p.ownedPlain, s.perKind[0] - p.perKind[0],
                 s.perKind[1] - p.perKind[1], s.prevValid - p.prevValid, s.blocks - p.blocks, s.prevNone - p.prevNone, s.prevStill - p.prevStill, s.notes - p.notes,
                 s.conflicts - p.conflicts, s.epochGaps - p.epochGaps);
    {
        // the last frame's wind registers against this one's (what the forms see): grass c12 (clock s, wind strength, bend weight, wave speed), trees c26 (rock / rustle angles)
        const WindHist& g = windHist_[1];
        const WindHist& t = windHist_[0];
        spdlog::info("native: tree motion: wind registers (prev -> cur): grass c12 ({:.4f} {:.4f} {:.4f} {:.4f}) -> ({:.4f} {:.4f} {:.4f} {:.4f}){}; trees c26 ({:.5f} {:.5f}) -> ({:.5f} {:.5f}){}",
                     g.prev[0][0], g.prev[0][1], g.prev[0][2], g.prev[0][3], g.cur[0][0], g.cur[0][1], g.cur[0][2], g.cur[0][3], g.prevValid ? "" : " (no history)", t.prev[14][0],
                     t.prev[14][1], t.cur[14][0], t.cur[14][1], t.prevValid ? "" : " (no history)");
    }
    if (windMode_ == 2) {
        spdlog::info("native: tree motion compare (5 s): {} vertices compared ({} not moved by the wind), the motion form's previous position vs F4-5's wind math "
                     "with the previous constants: {} above 0.01 px, max {:.5f} px / {:.6f} units; floor (this frame's constants vs the first run): {} above 0.01 px, max {:.5f} px / {:.6f} units{}",
                     s.compared - p.compared, s.compareStill - p.compareStill, s.compareOver - p.compareOver, s.winPx, s.winWorld, s.compareFloorOver - p.compareFloorOver, s.winFloorPx, s.winFloorWorld,
                     options_.backend == rhi::Backend::Vulkan ? "" : " (D3D12: no compare buffer)");
        s.winPx = s.winWorld = s.winFloorPx = s.winFloorWorld = 0.0f;
    }
    windLast_ = s;
}

}  // namespace rvk::d3d9
