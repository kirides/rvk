#include "enh/object_motion.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "enh/rt_scene.h"
#include "vk/frames.h"

namespace rvk::enh {

namespace {

using d3d9::draws::Kind;
using d3d9::scene::Pass;
using d3d9::scene::View;

constexpr std::uint32_t kMaxCapturesPerFrame = 256;
constexpr std::uint32_t kMaxSlots = 1u << 20;
constexpr std::uint32_t kRigidMaxSlots = 200000;
// the vertex shader constants that hold a static mesh's placement (g_vInstanceMisc c31, g_vInstanceData c32..)
constexpr std::uint32_t kRigidFirstReg = 31, kRigidRegs = 7;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v;
    return h * 1099511628211ull;
}

}  // namespace

ObjectMotion::ObjectMotion(d3d9::Device& device, RtScene* scene) : dev_(device), rt_(scene) {}

ObjectMotion::~ObjectMotion() { apply(false, false); }

void ObjectMotion::apply(bool skinned, bool rigid) {
    skinned_ = skinned;
    rigid_ = rigid;
    const bool want = (skinned || rigid) && dev_.ctx().features().vertexStores;
    if (want == registered_) return;
    registered_ = want;
    if (want) {
        // the same capture variant as the RT scene's (its SPIR-V cache tag too)
        dev_.setVariantProvider(d3d9::VariantSlot::Capture, {"cap5", RtScene::captureOptions});
        dev_.addDrawObserver(this);
        spdlog::info("enh: object motion: on (skinned {}, moving rigid draws {})", skinned ? "yes" : "no", rigid ? "yes" : "no");
    } else {
        dev_.removeDrawObserver(this);
        states_.clear();
        if ((skinned || rigid) && !dev_.ctx().features().vertexStores) {
            spdlog::warn("enh: object motion: the GPU's vertex shaders cannot write buffers: no per-object motion vectors");
        } else {
            spdlog::info("enh: object motion: off ({} captures, {} rigid draws seen moving)", captures_, rigidMoving_);
        }
    }
}

bool ObjectMotion::observes(Kind kind, d3d9::scene::Phase phase) const {
    if (phase.view != View::Main || phase.pass != Pass::Solid) return false;
    if (kind == Kind::Skinned) return skinned_ && !(rt_ != nullptr && rt_->capturingCharacters());
    return kind == Kind::StaticMesh && rigid_;
}

void ObjectMotion::bufferChanged(std::uint64_t bufferId) {
    std::erase_if(states_, [&](const auto& kv) { return kv.second.vb == bufferId || kv.second.ib == bufferId; });
}

void ObjectMotion::observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    // diagnostics: why rigid draws are not captured (logged with the capture stats)
    if (q.kind == Kind::StaticMesh) {
        ++diagRigid_;
        if (q.tag == nullptr || q.tag->entity == nullptr) ++diagNoEntity_;
        if (q.instanced) ++diagInstanced_;
        if ((q.indexBuffer != nullptr && q.indexBuffer->ringCopied()) || (q.vertexBuffer != nullptr && q.vertexBuffer->ringCopied())) {
            ++diagRing_;
        }
        const double t = dev.secondsSinceCreate();
        if (t - diagLog_ > 10.0) {
            diagLog_ = t;
            spdlog::info("enh: object motion: rigid draws seen {} (no entity {}, instanced {}, ring-copied {}), moving {}", diagRigid_,
                         diagNoEntity_, diagInstanced_, diagRing_, rigidMoving_);
        }
    }
    if (q.primitiveType != 4 || q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.primitives == 0 || q.instanced ||
        q.indexBuffer->ringCopied() || q.vertexBuffer->ringCopied()) {
        return;
    }
    const std::uint64_t frame = dev.frameValue();
    if (frame != frame_) {
        frame_ = frame;
        perFrame_ = 0;
        if (frame % 300 == 0) std::erase_if(states_, [&](const auto& kv) { return kv.second.seenFrame + 600 < frame; });
    }
    if (perFrame_ >= kMaxCapturesPerFrame) return;

    // a rigid draw without its entity cannot be told apart from the other objects sharing its mesh: they would feed each
    // other their previous positions (wrong, large motion on buildings)
    if (q.kind == Kind::StaticMesh && (q.tag == nullptr || q.tag->entity == nullptr)) return;
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
    s.seenFrame = frame;
    s.vb = vr.id;
    s.ib = ir.id;

    // what the last request became: it ran (the capture variant and its pipeline were ready), or it did not
    if (s.pending != 0) {
        if (s.pendingFrame + 1 == frame && dev.vertexCapturedPrev(s.pending)) {
            s.last = s.pending;
            s.lastSlots = s.pendingSlots;
            s.lastFrame = s.pendingFrame;
        }
        s.pending = 0;
    }

    if (q.kind == Kind::StaticMesh) {
        // a rigid draw is captured while its placement changes (and a few frames after)
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t r = 0; r < kRigidRegs; ++r) {
            const float* c = dev.vsConstant(kRigidFirstReg + r);
            std::uint32_t bits[4];
            std::memcpy(bits, c, sizeof(bits));
            for (std::uint32_t b : bits) h = mix(h, b);
        }
        if (s.haveHash && h != s.hash) {
            if (s.moving == 0) ++rigidMoving_;
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
    if (s.maxIndex == 0 || slots > (q.kind == Kind::StaticMesh ? kRigidMaxSlots : kMaxSlots)) return;
    const vk::RingAlloc a = dev.frames().allocate(16 + static_cast<VkDeviceSize>(slots) * 32, 16);
    if (a.ptr == nullptr) return;
    // header: slots, the previous frame's capture of this draw (its vertex shader reads the previous positions)
    std::uint32_t header[4] = {slots, 0, 0, 0};
    if (s.last != 0 && s.lastFrame + 1 == frame) {
        header[1] = static_cast<std::uint32_t>(s.last);
        header[2] = static_cast<std::uint32_t>(s.last >> 32);
        header[3] = s.lastSlots;
    }
    std::memcpy(a.ptr, header, 16);
    dev.requestVertexCapture(a.address);
    s.pending = a.address;
    s.pendingSlots = slots;
    s.pendingFrame = frame;
    ++perFrame_;
    ++captures_;

    const double now = dev.secondsSinceCreate();
    if (now - lastLog_ > 10.0) {
        lastLog_ = now;
        spdlog::info("enh: object motion: {} captures so far ({} rigid draws seen moving), {} tracked draws", captures_, rigidMoving_,
                     states_.size());
    }
}

}  // namespace rvk::enh
