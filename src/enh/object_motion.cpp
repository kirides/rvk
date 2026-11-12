#include "enh/object_motion.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "engine/scene_table.h"
#include "enh/rt_scene.h"

namespace rvk::enh {

namespace {

using d3d9::draws::Kind;
using d3d9::scene::Pass;
using d3d9::scene::View;

constexpr std::uint32_t kMaxCapturesPerFrame = 256;
constexpr std::uint32_t kMaxSlots = 1u << 20;
constexpr std::uint32_t kRigidMaxSlots = 200000;
// the vertex shader constants that hold a static mesh's placement: the world rows c33..c35 (g_vInstanceData[1..3]; an instanced mesh op's single placement too).
// (14.15: c31 / c32 / c36 / c37 were hashed as well: in Full the composed and owned draws' register file holds only the registers the draw's composition wrote (lean
// mode), the others are another draw's, so a still object looked "moving" whenever its neighbours in the list changed: ~80 false captures per frame at jungle)
constexpr std::uint32_t kRigidFirstReg = 33, kRigidRegs = 3;

// rigid draws: static meshes, and the "instanced mesh" op kind (held weapons / shields, nets, barrels: one placement per draw
// unless q.instanced, which is rejected)
bool isRigid(Kind k) { return k == Kind::StaticMesh || k == Kind::InstancedMesh; }

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v;
    return h * 1099511628211ull;
}

}  // namespace

ObjectMotion::ObjectMotion(d3d9::Device& device, RtScene* scene) : dev_(device), rt_(scene) {}

ObjectMotion::~ObjectMotion() {
    rt_ = nullptr;
    apply(false, false);
}

void ObjectMotion::apply(bool skinned, bool rigid) {
    skinned_ = skinned;
    rigid_ = rigid;
    if (rt_ != nullptr) rt_->setCharacterMotion(skinned);  // (the RT scene's solid-pass capture of a character is the motion source: it is kept while this is on)
    dev_.setRigidMotionWanted(rigid);  // (14.15 b: the static meshes' history forms only while moving rigid draws are asked for)
    const bool want = (skinned || rigid) && dev_.game().gameCaps().vertexStores;
    if (want == registered_) return;
    registered_ = want;
    if (want) {
        // the same capture variant as the RT scene's (its SPIR-V cache tag too)
        dev_.setVariantProvider(d3d9::VariantSlot::Capture, {d3d9::Device::captureTag(), RtScene::captureOptions});
        dev_.addDrawObserver(this);
        spdlog::info("enh: object motion: on (skinned {}, moving rigid draws {})", skinned ? "yes" : "no", rigid ? "yes" : "no");
    } else {
        dev_.removeDrawObserver(this);
        states_.clear();
        if ((skinned || rigid) && !dev_.game().gameCaps().vertexStores) {
            spdlog::warn("enh: object motion: the GPU's vertex shaders cannot write buffers: no per-object motion vectors");
        } else {
            spdlog::info("enh: object motion: off ({} captures, {} rigid draws seen moving)", captures_, rigidMoving_);
        }
    }
}

bool ObjectMotion::observes(Kind kind, d3d9::scene::Phase phase) const {
    if (phase.view != View::Main || phase.pass != Pass::Solid) return false;
    // (trace level: also the draws the RT scene captures, to log them in observe())
    if (kind == Kind::Skinned) {
        return skinned_ && (!(rt_ != nullptr && rt_->capturingCharacters()) || spdlog::should_log(spdlog::level::trace));
    }
    return (kind == Kind::StaticMesh || kind == Kind::InstancedMesh) && rigid_;
}

void ObjectMotion::bufferChanged(std::uint64_t bufferId) {
    std::erase_if(states_, [&](const auto& kv) { return kv.second.vb == bufferId || kv.second.ib == bufferId; });
}

void ObjectMotion::observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    // diagnostics: why rigid draws are not captured (logged with the capture stats)
    if (isRigid(q.kind)) {
        ++diagRigid_;
        if (q.tag == nullptr || q.tag->entity == nullptr) ++diagNoEntity_;
        if (q.instanced) ++diagInstanced_;
        if ((q.indexBuffer != nullptr && q.indexBuffer->ringCopied()) || (q.vertexBuffer != nullptr && q.vertexBuffer->ringCopied())) {
            ++diagRing_;
        }
        const double t = dev.secondsSinceCreate();
        if (t - diagLog_ > 10.0) {
            diagLog_ = t;
            spdlog::info("enh: object motion: rigid draws seen {} (no entity {}, instanced {}, ring-copied {}), moving {}, capture cap hit {}x", diagRigid_,
                         diagNoEntity_, diagInstanced_, diagRing_, rigidMoving_, diagCapped_);
        }
    }
    if (q.kind == Kind::Skinned) {
        // skinned draws rejected before they are tracked (a held weapon / shield may be drawn from a dynamic buffer, as strips, ...)
        ++diagSkin_;
        const bool noBuf = q.vertexBuffer == nullptr || q.indexBuffer == nullptr;
        if (q.primitiveType != 4) ++diagSkinType_;
        if (noBuf || q.primitives == 0) ++diagSkinNoBuf_;
        if (q.instanced) ++diagSkinInst_;
        if (!noBuf && (q.indexBuffer->ringCopied() || q.vertexBuffer->ringCopied())) {
            ++diagSkinRing_;
            if (diagSkinRing_ <= 40 && spdlog::should_log(spdlog::level::trace)) {
                spdlog::trace("enh: object motion: skinned draw from a ring-copied buffer: entity {} prims {} vb {} ib {}",
                              static_cast<const void*>(q.tag != nullptr ? q.tag->entity : nullptr), q.primitives,
                              q.vertexBuffer->ringCopied(), q.indexBuffer->ringCopied());
            }
        }
        const double t = dev.secondsSinceCreate();
        if (t - diagSkinLog_ > 10.0) {
            diagSkinLog_ = t;
            spdlog::info("enh: object motion: skinned draws seen {} (not triangle lists {}, no buffers {}, instanced {}, ring-copied {})", diagSkin_,
                         diagSkinType_, diagSkinNoBuf_, diagSkinInst_, diagSkinRing_);
        }
    }
    // 14.13 (native.bone_motion): the draw takes the bone-motion forms: its previous positions come from the bone table's history, no capture
    // (14.15 b, native.rigid_motion: a static mesh's draw with the object table's history likewise)
    // (14.22: a rigid draw whose VS window 2 is an instance row image has no motion of its own on any path: the image is immutable while its record lives)
    if ((q.kind == Kind::Skinned || isRigid(q.kind)) && (dev.boneMotionCovered() || (isRigid(q.kind) && dev.rigidRowWindow()))) {
        ++boneDraws_;
        dev.noteCaptureAvoided(q.kind == Kind::Skinned ? d3d9::Device::CaptureBy::MotionSkinned : d3d9::Device::CaptureBy::MotionRigid, d3d9::Device::captureKindOf(q));
        return;
    }
    if (q.primitiveType != 4 || q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.primitives == 0 || q.instanced ||
        q.indexBuffer->ringCopied() || q.vertexBuffer->ringCopied()) {
        return;
    }
    // the memory epoch (GameDevice::memoryEpoch), not the frame value: the previous frame's capture stays usable across a mid-frame read-back flush (its ring
    // memory lives through the next epoch; frame-ownership.md 14.40)
    const std::uint64_t frame = dev.game().memoryEpoch();
    if (frame != frame_) {
        frame_ = frame;
        perFrame_ = 0;
        if (frame % 300 == 0) std::erase_if(states_, [&](const auto& kv) { return kv.second.seenFrame + 600 < frame; });
    }
    if (perFrame_ >= kMaxCapturesPerFrame) {
        ++diagCapped_;
        return;
    }

    // a rigid draw without its entity cannot be told apart from the other objects sharing its mesh: they would feed each
    // other their previous positions (wrong, large motion on buildings)
    if (isRigid(q.kind) && (q.tag == nullptr || q.tag->entity == nullptr)) return;
    const d3d9::ResourceRef vr = q.vertexBuffer->ref(), ir = q.indexBuffer->ref();
    std::uint64_t key = 1469598103934665603ull;
    key = mix(key, reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->entity : nullptr));
    key = mix(key, vr.id);
    key = mix(key, vr.generation);
    key = mix(key, ir.id);
    key = mix(key, ir.generation);
    key = mix(key, q.first);
    key = mix(key, q.primitives);
    key = mix(key, static_cast<std::uint32_t>(q.baseVertex));
    key = mix(key, q.vertexOffset);
    State& s = states_[key];
    if (s.seenFrame == frame) return;  // the later passes of a draw: the first one counts
    if (s.seenFrame == 0 && spdlog::should_log(spdlog::level::trace)) {
        spdlog::trace("enh: object motion: new draw: kind {} entity {} material '{}' prims {}", d3d9::draws::name(q.kind),
                      static_cast<const void*>(q.tag != nullptr ? q.tag->entity : nullptr),
                      q.tag != nullptr && q.tag->materialName != nullptr ? q.tag->materialName : "?", q.primitives);
    }
    if (q.kind == Kind::Skinned && rt_ != nullptr && rt_->capturingCharacters()) {
        s.seenFrame = frame;  // the RT scene captures it (trace only: the log above)
        return;
    }
    s.seenFrame = frame;
    s.vb = vr.id;
    s.ib = ir.id;

    // what the last request became: it ran (the capture variant and its pipeline were ready), or it did not
    if (!s.pending.null()) {
        if (s.pendingFrame + 1 == frame && dev.vertexCapturedPrev(s.pending)) {
            s.last = s.pending;
            s.lastSlots = s.pendingSlots;
            s.lastFrame = s.pendingFrame;
        }
        s.pending = {};
    }

    if (isRigid(q.kind)) {
        // a rigid draw is captured while its placement changes (and a few frames after)
        std::uint64_t h = 1469598103934665603ull;
        const bool old = d3d9::Device::nocapOld();  // (the hash before 14.15: c31..c37 of the register file)
        for (std::uint32_t r = 0; r < (old ? 7u : kRigidRegs); ++r) {
            // (an owned draw's rows as the object table holds them: its register file is not loaded for every register)
            const float* c = old ? dev.vsConstant(31 + r) : q.objectRows != nullptr ? q.objectRows + 4 * r : dev.vsConstant(kRigidFirstReg + r);
            std::uint32_t bits[4];
            std::memcpy(bits, c, sizeof(bits));
            for (std::uint32_t b : bits) h = mix(h, b);
        }
        if (s.haveHash && h != s.hash) {
            if (s.moving == 0) {
                ++rigidMoving_;
                if (spdlog::should_log(spdlog::level::trace)) {
                    spdlog::trace("enh: object motion: rigid draw starts moving: entity {} material '{}' prims {}",
                                  static_cast<const void*>(q.tag->entity), q.tag->materialName != nullptr ? q.tag->materialName : "?",
                                  q.primitives);
                }
            }
            s.moving = 4;
        } else if (s.moving > 0) {
            --s.moving;
        }
        s.hash = h;
        s.haveHash = true;
        if (s.moving == 0) return;
    }

    d3d9::Buffer* ib = q.indexBuffer;
    const bool index32 = ib->index32();
    const std::uint32_t isize = index32 ? 4u : 2u;
    if (!s.scanned) {
        // the highest index of the range = the capture's slot count
        s.scanned = true;
        std::vector<std::uint8_t> idx(static_cast<std::size_t>(q.primitives) * 3 * isize);
        if (ib->readCpu(q.first * isize, static_cast<std::uint32_t>(idx.size()), idx.data())) {
            std::uint32_t m = 0;
            for (std::size_t i = 0; i < idx.size(); i += isize) {
                std::uint32_t v = 0;
                std::memcpy(&v, idx.data() + i, isize);
                m = std::max(m, v);
            }
            s.maxIndex = m;
        }
    }
    const std::uint32_t slots = s.maxIndex + 1;
    if (s.maxIndex == 0 || slots > (isRigid(q.kind) ? kRigidMaxSlots : kMaxSlots)) return;
    const rhi::RingAlloc a = dev.game().allocCapture(16 + static_cast<std::uint64_t>(slots) * 32, 16);
    if (a.ptr == nullptr) return;
    // header: slots, the previous frame's capture of this draw (its vertex shader reads the previous positions)
    std::uint32_t header[4] = {slots, 0, 0, 0};
    {  // (14.15 b: rigid draws too)
        // native.bone_motion=compare: the capture vertex shader also runs the bone path on this draw's bone image and compares (bit 30 of the slots)
        const std::uint32_t bit = dev.boneCompareBit();
        header[0] |= bit;
        if (bit != 0) ++dev.boneStats.compareDraws;
    }
    if (!s.last.null() && s.lastFrame + 1 == frame) {
        header[1] = s.last.low();
        header[2] = s.last.high();
        header[3] = s.lastSlots;
    }
    std::memcpy(a.ptr, header, 16);
    if (isRigid(q.kind)) dev.noteRigidCapture(dev.ownedVsWindowKind(2) == d3d9::OwnedWindows::Kind::Object, engine::scenetable::find(q.tag->entity) != UINT32_MAX);
    dev.requestVertexCapture(a, false, isRigid(q.kind) ? d3d9::Device::CaptureBy::MotionRigid : d3d9::Device::CaptureBy::MotionSkinned, d3d9::Device::captureKindOf(q));
    s.pending = a.ref;
    s.pendingSlots = slots;
    s.pendingFrame = frame;
    ++perFrame_;
    ++captures_;

    const double now = dev.secondsSinceCreate();
    if (now - lastLog_ > 10.0) {
        lastLog_ = now;
        spdlog::info("enh: object motion: {} captures so far ({} rigid draws seen moving), {} tracked draws, {} skinned draws without a capture (bone-history motion)", captures_,
                     rigidMoving_, states_.size(), boneDraws_);
    }
}

}  // namespace rvk::enh
