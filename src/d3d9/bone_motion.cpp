// Bone-history motion (native.bone_motion, docs/research/frame-ownership.md 14.13). Per-object motion of the skinned characters without vertex captures: the bone
// table (gpuscene/bone_table) keeps each part's image of the previous frame at a fixed byte delta from the current one; the bone-motion vertex shader forms (VariantSlot
// BoneVs -> SceneVsBone / SceneVsBone2 / SceneVsBonePull) run the translated program a second time with VS window 2 pointed at that image (RVK_BONE_PREV,
// rvk_draw.hlsli) and output the previous world position (rvk_prevWorld, location 15) that the G-buffer motion pixel variants (GbufferMotion / -M, scene forms
// SceneGbufferMotion*) turn into the motion vector, exactly as they do for the capture variant's. The first run is the translated program unchanged: the clip position
// stays bit-identical to every other form of the shader (z-equal colour pass, shader-permutations.md 12). The record's prevVs carries the history image's base (toScenePacket /
// ownedBuild: only a base the bone table handed out gets one), 0 = no motion of its own (the camera's).
// Compare mode: the captures stay the motion source, and the capture vertex shader of a draw whose window 2 is a bone image with history (header bit 30) also runs the
// bone path and accumulates the difference of both previous positions (world units and pixels on the previous screen) into a Readback buffer (frame param 6).

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "gpuscene/bone_table.h"
#include "gpuscene/object_table.h"
#include "shader/abi.h"

namespace rvk::d3d9 {

namespace {

constexpr std::uint32_t kPrevWorldLocation = 15;  // enh::kPrevWorldLocation (the capture variant's / the motion pixel variants' varying)
constexpr std::uint32_t kCompareBit = 0x40000000u;  // capture header x: the draw's window 2 is a bone image with history (compare mode)
constexpr std::uint32_t kCmpSlots = rhi::kFramesInFlight + 2;
constexpr std::uint32_t kCmpBytes = 32;  // per slot: max px bits, compared, max world bits, over 0.01 px, no previous capture

// the second run of the translated program on the previous frame's bone image (scene forms: the record's prevVs is that image's window base)
const char* kBoneEpilogue =
    "    rvk_out.rvk_prevWorld = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    {\n"
    "        // bone-history motion (native.bone_motion, frame-ownership.md 14.13): the previous frame's bone image (prevVs, 0 = none), its c231.x = 1 when valid\n"
    "        const uint2 rvk_bmRef = rvkDrawPrevVs();\n"
    "        const bool rvk_bmHas = any(rvk_bmRef != uint2(0u, 0u));\n"
    "        const uint2 rvk_bmPrev = rvk_bmHas ? rvk_bmRef : rvkVsWin(2u);  // (never the zero pair: a load hoisted above the test stays in a live buffer)\n"
    "        // (the validity word, RVK_BM_VALID: a bone image's c231.x = 1; a rigid draw's object history record (14.15 b): its info.w (c36.w) = 1)\n"
    "        const bool rvk_bmOk = RVK_BM_VALID(rvk_bmPrev);\n"
    "        if (rvk_bmHas && rvk_bmOk) {\n"
    "            rvk_vsw2Ov = rvk_bmPrev;\n"
    "            rvk_resetRegs();\n"
    "            rvk_main();\n"
    "            rvk_vsw2Ov = uint2(0u, 0u);\n"
    "            // (precise like the first run's position: the same operations in the same order, so the clip position of the previous image is what the first run gives\n"
    "            // for it, bit for bit; back to the world as the capture variant does, through the inverse view-projection)\n"
    "            precise float4 rvk_bmP = $pos;\n"
    "            const float4 rvk_bmW = mul(rvk_bmP, rvkLoadFrame(pc.frame).invViewProj);\n"
    "            if (abs(rvk_bmW.w) > 1e-20) rvk_out.rvk_prevWorld = float4(rvk_bmW.xyz / rvk_bmW.w, 1.0);\n"
    "        }\n"
    "    }";

std::atomic<bool> g_compare{false};

}  // namespace

bool Device::boneVsOptions(const Shader& s, shader::TranslateOptions& o) {
    if (s.stage != shader::Stage::Vertex) return false;
    o.defines += "#define RVK_BONE_PREV 1\n";
    // (the validity word: a skinned shader (relative addressing into its palette) reads a bone image, c231.x; a rigid one an object record, c36.w = info.w; 14.15 b)
    o.defines += s.reflection.relativeConstants ? "#define RVK_BM_VALID(b) (rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(b), 231u * 16u)).x == 1.0)\n"
                                                : "#define RVK_BM_VALID(b) (rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(b), 36u * 16u)).w == 1u)\n";
    o.rerunnable = true;
    o.extraOutputs += std::format("    [[vk::location({})]] float4 rvk_prevWorld : VARY{};", kPrevWorldLocation, kPrevWorldLocation);
    o.epilogue += kBoneEpilogue;
    return true;
}

// compare mode: appended to the capture variant (enh/rt_scene captureOptions, push form: pc.prevVs = the capture, pc.vsw2 = the bone image). The capture's previous world
// position (rvk_prevWorld, from the previous frame's capture) against the bone path's (the image at the history delta, when its c231.x = 1)
void Device::boneCompareText(shader::TranslateOptions& o, const Shader& s) {
    if (!g_compare.load(std::memory_order_relaxed)) return;
    // (the header's slot count keeps bit 30 out of it)
    for (std::size_t at = o.epilogue.find("0x7FFFFFFFu"); at != std::string::npos; at = o.epilogue.find("0x7FFFFFFFu", at)) o.epilogue.replace(at, 11, "0x3FFFFFFFu");
    o.defines += "#define RVK_BONE_PREV 1\n";
    o.rerunnable = true;
    o.epilogue += std::format(
        "\n    {{\n"
        "        // native.bone_motion=compare (frame-ownership.md 14.13)\n"
        "        const uint2 rvk_bcAddr = rvkDrawPrevVs();\n"
        "        if (any(rvk_bcAddr != uint2(0u, 0u))) {{\n"
        "            const uint4 rvk_bcInfo = rvkRwLoad<uint4, 16>(rvkAddr(rvk_bcAddr));\n"
        "            const RvkFrame rvk_bcF = rvkLoadFrame(pc.frame);\n"
        "            const uint2 rvk_bcBuf = uint2(asuint(rvk_bcF.params[{}].x), asuint(rvk_bcF.params[{}].y));\n"
        "            const bool rvk_bcOn = (rvk_bcInfo.x & 0x40000000u) != 0u && any(rvk_bcBuf != uint2(0u, 0u)) && rvk_bcF.params[{}].z == 1.0;\n"
        "            const uint2 rvk_bcCur = rvkVsWin(2u);\n"
        "            const uint2 rvk_bcPrev = rvk_bcOn ? rvkPairAdd(rvk_bcCur, {}u) : rvk_bcCur;  // (the bone table's history delta; the current image when off)\n"
        "            const bool rvk_bcOk = {};\n"
        "            if (rvk_bcOn && rvk_bcOk) {{\n"
        "                const RvkAddr rvk_bcE = rvkAddr(rvk_bcOn ? rvk_bcBuf : rvk_bcAddr);\n"
        "                if (rvk_out.rvk_prevWorld.w == 1.0) {{\n"
        "                    // (every run's clip position precise, back to the world through the inverse view-projection: what the capture variant does)\n"
        "                    const float4 rvk_bcOp1 = mul(rvk_pos, rvk_bcF.invViewProj);\n"
        "                    precise float4 rvk_bcP2 = float4(0.0, 0.0, 0.0, 0.0);\n"
        "                    rvk_vsw2Ov = rvk_bcPrev;\n"
        "                    rvk_resetRegs();\n"
        "                    rvk_main();\n"
        "                    rvk_vsw2Ov = uint2(0u, 0u);\n"
        "                    rvk_bcP2 = $pos;\n"
        "                    const float4 rvk_bcW = mul(rvk_bcP2, rvk_bcF.invViewProj);\n"
        "                    if (abs(rvk_bcW.w) > 1e-20) {{\n"
        "                        const float3 rvk_bcB = rvk_bcW.xyz / rvk_bcW.w;\n"
        "                        const float3 rvk_bcA = rvk_out.rvk_prevWorld.xyz;\n"
        "                        const float4 rvk_bcPa = mul(float4(rvk_bcA, 1.0), rvk_bcF.prevViewProj);\n"
        "                        const float4 rvk_bcPb = mul(float4(rvk_bcB, 1.0), rvk_bcF.prevViewProj);\n"
        "                        float rvk_bcPx = 0.0;\n"
        "                        if (rvk_bcPa.w > 1e-4 && rvk_bcPb.w > 1e-4) {{\n"
        "                            const float2 rvk_bcD = (rvk_bcPa.xy / rvk_bcPa.w - rvk_bcPb.xy / rvk_bcPb.w) * 0.5 * rvk_bcF.viewport.xy;\n"
        "                            rvk_bcPx = length(rvk_bcD);\n"
        "                        }}\n"
        "                        rvkRwAtomicMax(rvk_bcE, asuint(rvk_bcPx));\n"
        "                        rvkRwAtomicAdd(rvkAddrAdd(rvk_bcE, 4u), 1u);\n"
        "                        rvkRwAtomicMax(rvkAddrAdd(rvk_bcE, 8u), asuint(length(rvk_bcA - rvk_bcB)));\n"
        "                        if (rvk_bcPx > 0.01) rvkRwAtomicAdd(rvkAddrAdd(rvk_bcE, 12u), 1u);\n"
        "                        // (the motion each path sees: its previous position against this frame's, world units)\n"
        "                        const float3 rvk_bcC = rvk_bcOp1.xyz / rvk_bcOp1.w;\n"
        "                        rvkRwAtomicMax(rvkAddrAdd(rvk_bcE, 20u), asuint(length(rvk_bcB - rvk_bcC)));\n"
        "                        rvkRwAtomicMax(rvkAddrAdd(rvk_bcE, 24u), asuint(length(rvk_bcA - rvk_bcC)));\n"
        "                        // (the second run itself: on this frame's image it must give this frame's position)\n"
        "                        precise float4 rvk_bcP3 = float4(0.0, 0.0, 0.0, 0.0);\n"
        "                        rvk_vsw2Ov = rvk_bcCur;\n"
        "                        rvk_resetRegs();\n"
        "                        rvk_main();\n"
        "                        rvk_vsw2Ov = uint2(0u, 0u);\n"
        "                        rvk_bcP3 = $pos;\n"
        "                        const float4 rvk_bcR4 = mul(rvk_bcP3, rvk_bcF.invViewProj);\n"
        "                        rvkRwAtomicMax(rvkAddrAdd(rvk_bcE, 28u), asuint(length(rvk_bcR4.xyz / rvk_bcR4.w - rvk_bcC)));\n"
        "                    }}\n"
        "                }} else {{\n"
        "                    rvkRwAtomicAdd(rvkAddrAdd(rvk_bcE, 16u), 1u);\n"
        "                }}\n"
        "            }}\n"
        "        }}\n"
        "    }}",
        shader::abi::kFrameParamBoneMotion, shader::abi::kFrameParamBoneMotion, shader::abi::kFrameParamBoneMotion,
        // (14.15 b: a rigid shader's window 2 is an object record: the object table's history delta and validity word)
        s.reflection.relativeConstants ? gpuscene::BoneTable::kHistoryDelta : gpuscene::ObjectTable::kHistoryDelta,
        s.reflection.relativeConstants ? "rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvk_bcPrev), 231u * 16u)).x == 1.0"
                                       : "rvkRawLoad<uint4, 16>(rvkAddrAdd(rvkAddr(rvk_bcPrev), 36u * 16u)).w == 1u");
}

void Device::setBoneMotion(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    if (mode == boneMode_) return;
    boneMode_ = mode;
    ++variantEpoch_;
    g_compare.store(mode == 2, std::memory_order_relaxed);
    if (mode == 1) setVariantProvider(VariantSlot::BoneVs, {"bmot6", boneVsOptions});
    if (mode == 2) {
        // the capture variant (registered by enh/object_motion / enh/rt_scene) gets the compare text under its own cache tag
        VariantProvider cap;
        {
            const std::lock_guard lock(variantMutex_);
            cap = variantProviders_[static_cast<std::size_t>(VariantSlot::Capture)];
        }
        if (cap.options) setVariantProvider(VariantSlot::Capture, {captureTag(), cap.options});
    }
    if (mode != 1) bonePipes_.clear();
    spdlog::info("native: bone motion {}", mode == 1 ? "on (the skinned solid draws' motion from the bone table's history)" : mode == 2 ? "compare (captures + GPU compare)" : "off");
}

Device::CaptureKind Device::captureKindOf(const ReplaceQuery& q) {
    switch (q.kind) {
        case draws::Kind::StaticMesh: return CaptureKind::Static;
        case draws::Kind::InstancedMesh: return CaptureKind::Instanced;
        case draws::Kind::SpeedTree: return CaptureKind::SpeedTree;
        case draws::Kind::SpeedTreeBillboard:
        case draws::Kind::Billboard: return CaptureKind::Billboard;
        case draws::Kind::Vegetation: return CaptureKind::Vegetation;
        case draws::Kind::Skinned: {
            if (q.decl != nullptr) {
                for (const VertexElement& e : q.decl->elements()) {
                    if (e.stream == kDeclEndStream) break;
                    if (e.stream != 0 || (e.usage == 0 && e.usageIndex != 0)) return CaptureKind::Morph;  // (POSITION1..3 in streams 1..3: the heads' morph targets)
                }
            }
            return CaptureKind::Skinned;
        }
        default: return CaptureKind::Other;
    }
}

void Device::captureCensusFrame() {
    ++captureCensusFrames_;
    const double now = secondsSinceCreate();
    if (now - captureCensusLog_ < 5.0) return;
    const bool first = captureCensusLog_ < -1e8;
    captureCensusLog_ = now;
    static constexpr const char* kBy[] = {"motion skinned", "motion rigid", "rt solid without a skin job", "rt solid for motion", "rt cascade", "rt skin verify", "rt tree", "other"};
    static constexpr const char* kKind[] = {"static", "instanced mesh", "skinned", "morph", "speedtree", "vegetation", "billboard", "other"};
    static constexpr const char* kSite[] = {"composed", "retained", "owned"};
    std::string rows;
    std::uint64_t total = 0, totalAvoided = 0;
    for (std::size_t b = 0; b < static_cast<std::size_t>(CaptureBy::Count); ++b) {
        for (std::size_t k = 0; k < static_cast<std::size_t>(CaptureKind::Count); ++k) {
            std::uint64_t n[static_cast<std::size_t>(CaptureSite::Count)];
            std::uint64_t sum = 0;
            for (std::size_t s = 0; s < static_cast<std::size_t>(CaptureSite::Count); ++s) {
                n[s] = captureCensus_.n[b][k][s] - captureCensusLast_.n[b][k][s];
                sum += n[s];
            }
            const std::uint64_t av = captureCensus_.avoided[b][k] - captureCensusLast_.avoided[b][k];
            total += sum;
            totalAvoided += av;
            if (sum == 0 && av == 0) continue;
            rows += std::format("; {} x {}: {}", kBy[b], kKind[k], sum);
            if (sum != 0) {
                rows += " (";
                bool any = false;
                for (std::size_t s = 0; s < static_cast<std::size_t>(CaptureSite::Count); ++s) {
                    if (n[s] == 0) continue;
                    rows += std::format("{}{} {}", any ? ", " : "", kSite[s], n[s]);
                    any = true;
                }
                rows += ")";
            }
            if (av != 0) rows += std::format(" avoided {}", av);
        }
    }
    std::string win;
    for (std::size_t b = 0; b < static_cast<std::size_t>(CaptureBy::Count); ++b) {
        const std::uint64_t u = captureCensus_.win2[b][0] - captureCensusLast_.win2[b][0], o = captureCensus_.win2[b][1] - captureCensusLast_.win2[b][1],
                            bo = captureCensus_.win2[b][2] - captureCensusLast_.win2[b][2];
        if (u + o + bo != 0) win += std::format("{}{}: uploaded {} / object record {} / bone image {}", win.empty() ? "" : ", ", kBy[b], u, o, bo);
    }
    const std::uint64_t frames = std::max<std::uint64_t>(captureCensusFrames_ - captureCensusFramesLast_, 1);
    if (!first || total != 0) {
        const std::uint64_t r0 = captureCensus_.rigid[0] - captureCensusLast_.rigid[0], r1 = captureCensus_.rigid[1] - captureCensusLast_.rigid[1],
                            r2 = captureCensus_.rigid[2] - captureCensusLast_.rigid[2];
        spdlog::info("native: captures (5 s): {} vertex-capture requests ({:.1f} per frame), {} left out for a capture-free source{}; moving rigid captures: window 2 an object record {}, uploaded {}, entity with a scene-table slot {}; VS window 2 per requester: {}", total,
                     static_cast<double>(total) / static_cast<double>(frames), totalAvoided, rows.empty() ? std::string("; none") : rows, r0, r1, r2, win.empty() ? std::string("-") : win);
    }
    captureCensusLast_ = captureCensus_;
    captureCensusFramesLast_ = captureCensusFrames_;
}

bool Device::boneCompareOn() { return g_compare.load(std::memory_order_relaxed); }
void Device::boneCompareForce(bool on) { g_compare.store(on, std::memory_order_relaxed); }

std::uint32_t Device::boneCompareBit() const {
    if (boneMode_ != 2 || owned_ == nullptr) return 0;
    const OwnedWindows::Kind w2 = owned_->w[0][2].kind;
    if (w2 != OwnedWindows::Kind::Bones && !(w2 == OwnedWindows::Kind::Object && rigidMode_)) return 0;  // (14.15 b: a rigid draw's object record too)
    // (the compare text's validity word / delta follow the shader's addressing: it must match the window's table, as in bonePredict)
    if (vs_.get() == nullptr || vs_->reflection.relativeConstants != (w2 == OwnedWindows::Kind::Bones)) return 0;
    if (options_.backend != rhi::Backend::Vulkan) return 0;  // (the Readback buffer is reached by address on Vulkan only)
    const scene::Phase ph = phases_.current();
    if (ph.view != scene::View::Main || ph.pass != scene::Pass::Solid) return 0;
    return historyOf(owned_->w[0][2].base).null() ? 0 : kCompareBit;
}

bool Device::boneFormsReady() {
    if (vs_.get() == nullptr || ps_.get() == nullptr) return false;
    const VariantSlot fs = sceneSlotOf(gbufferSlot(true));
    if (fs == VariantSlot::Count) return false;
    const bool v2 = sceneV2_;
    // (all requested at once: the first frames of a part build them in the background)
    const rhi::ShaderModule a = variant(vs_, VariantSlot::SceneVsBone);
    const rhi::ShaderModule b = variant(ps_, fs);
    const rhi::ShaderModule c = v2 ? variant(vs_, VariantSlot::SceneVsBone2) : rhi::ShaderModule::None;
    const rhi::ShaderModule d = v2 ? variant(ps_, sceneView2Of(fs)) : rhi::ShaderModule::None;
    return a != rhi::ShaderModule::None && b != rhi::ShaderModule::None && (!v2 || (c != rhi::ShaderModule::None && d != rhi::ShaderModule::None));
}

rhi::GpuRef Device::historyOf(rhi::GpuRef base) const {
    const rhi::GpuRef b = gpuscene::bonetable().prevOf(base);
    if (!b.null() || !rigidMode_) return b;
    return gpuscene::objecttable().prevOf(base);
}

bool Device::bonePredict(draws::Kind kind, const scene::Phase& phase) {
    if (boneMode_ != 1 || phase.view != scene::View::Main || phase.pass != scene::Pass::Solid) return false;
    const bool skinned = kind == draws::Kind::Skinned;
    const bool rigid = kind == draws::Kind::StaticMesh && rigidMotionOn();  // 14.15 b
    if (!skinned && !rigid) return false;
    if (!sceneActive() || !gbuf_.active || gbuf_.motion == nullptr || owned_ == nullptr || vs_.get() == nullptr) return false;
    // (the forms' validity word is chosen per shader (boneVsOptions): a palette-indexing shader reads a bone image's c231, any other an object record's c36; a shader
    // whose addressing does not match its window's table never takes the forms: an object record read at c231 would leave the record, GPU safety)
    if (vs_->reflection.relativeConstants != skinned) {
        if (rigid) ++boneStats.rigidNoWindow;
        return false;
    }
    if (skinned) {
        if (!sceneActors_ || owned_->w[0][2].kind != OwnedWindows::Kind::Bones) return false;
        if (gpuscene::bonetable().prevOf(owned_->w[0][2].base).null()) {
            ++boneStats.noHistory;
            return false;
        }
    } else {
        if (owned_->w[0][2].kind != OwnedWindows::Kind::Object) {
            ++boneStats.rigidNoWindow;
            return false;
        }
        if (gpuscene::objecttable().prevOf(owned_->w[0][2].base).null()) {
            ++boneStats.rigidNoHistory;
            return false;
        }
    }
    if (!boneFormsReady()) {
        ++(skinned ? boneStats.pending : boneStats.rigidPending);
        return false;
    }
    ++(skinned ? boneStats.predicted : boneStats.rigidPredicted);
    return true;
}

void Device::boneMotionFrame() {
    if (boneMode_ == 0) return;
    BoneMotionStats& s = boneStats;
    if (boneMode_ == 2 && options_.backend == rhi::Backend::Vulkan) {
        if (boneCmp_ == nullptr && rhi() != nullptr) {
            boneCmp_ = rhi()->createBuffer({static_cast<std::uint64_t>(kCmpBytes) * kCmpSlots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Readback, "bone motion compare"});
            if (boneCmp_ != nullptr && boneCmp_->map() != nullptr) std::memset(boneCmp_->map(), 0, static_cast<std::size_t>(kCmpBytes) * kCmpSlots);
        }
        if (boneCmp_ != nullptr && boneCmp_->map() != nullptr) {
            // the slot of the next frame: its last use was kCmpSlots frames ago (> the frames in flight: the GPU is done with it)
            const std::uint32_t slot = static_cast<std::uint32_t>(++boneCmpFrame_ % kCmpSlots);
            boneCmp_->invalidate();
            auto* e = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(boneCmp_->map()) + static_cast<std::size_t>(kCmpBytes) * slot);
            float px = 0.0f, world = 0.0f;
            std::memcpy(&px, &e[0], 4);
            std::memcpy(&world, &e[2], 4);
            s.compared += e[1];
            s.compareOver += e[3];
            float mb = 0.0f, mc = 0.0f, rr = 0.0f;
            std::memcpy(&mb, &e[5], 4);
            std::memcpy(&mc, &e[6], 4);
            std::memcpy(&rr, &e[7], 4);
            s.winRerun = std::max(s.winRerun, rr);
            s.winMotionBone = std::max(s.winMotionBone, mb);
            s.winMotionCapture = std::max(s.winMotionCapture, mc);
            s.compareMiss += e[4];
            s.maxPx = std::max(s.maxPx, px);
            s.maxWorld = std::max(s.maxWorld, world);
            s.winPx = std::max(s.winPx, px);
            s.winWorld = std::max(s.winWorld, world);
            std::memset(e, 0, kCmpBytes);
            boneCmp_->flush();
            const std::array<std::uint32_t, 2> ref = boneCmp_->shaderRef(static_cast<std::uint64_t>(kCmpBytes) * slot);
            setFrameParam(shader::abi::kFrameParamBoneMotion, {std::bit_cast<float>(ref[0]), std::bit_cast<float>(ref[1]), 1.0f, 0.0f});
        }
    }
    const double now = secondsSinceCreate();
    if (now - boneLog_ < 5.0) return;
    boneLog_ = now;
    const BoneMotionStats& p = boneLast_;
    if (boneMode_ == 1) {
        spdlog::info("native: bone motion (5 s): skinned solid draws predicted {} (forms pending {}, no history {}), drawn with the bone-motion forms: composed {} (v2 retained {}, pulled {}), "
                     "owned records with a history base {} (without {}), {} bone-motion pipelines",
                     s.predicted - p.predicted, s.pending - p.pending, s.noHistory - p.noHistory, s.chosen - p.chosen, s.chosen2 - p.chosen2, s.chosenPull - p.chosenPull,
                     s.records - p.records, s.recordsNoPrev - p.recordsNoPrev, bonePipes_.size());
        if (rigidMode_) {
            spdlog::info("native: rigid motion (5 s): {}; static solid draws predicted {} (no object window {}, no history {}, forms pending {})",
                         rigidMotionOn() ? "on" : "waiting for upscale.object_motion=all", s.rigidPredicted - p.rigidPredicted, s.rigidNoWindow - p.rigidNoWindow,
                         s.rigidNoHistory - p.rigidNoHistory, s.rigidPending - p.rigidPending);
        }
    } else {
        spdlog::info("native: bone motion compare (5 s): {} capture draws with a bone image; vertices compared {}, max error {:.5f} px / {:.6f} units, {} above 0.01 px, {} without a previous capture; largest motion seen: bone path {:.4f}, captures {:.4f} units; second run on this frame's image vs the first: {:.6f} units",
                     s.compareDraws - p.compareDraws, s.compared - p.compared, s.winPx, s.winWorld, s.compareOver - p.compareOver, s.compareMiss - p.compareMiss, s.winMotionBone, s.winMotionCapture, s.winRerun);
        s.winPx = s.winWorld = s.winMotionBone = s.winMotionCapture = s.winRerun = 0.0f;
    }
    boneLast_ = s;
}

}  // namespace rvk::d3d9
