#include "enh/rt_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "vk/context.h"
#include "vk/frames.h"

namespace rvk::enh {

namespace abi = shader::abi;
using d3d9::draws::Kind;
using d3d9::scene::Pass;
using d3d9::scene::View;

namespace {

constexpr std::uint8_t kDeclFloat1 = 0, kDeclFloat3 = 2;  // D3DDECLTYPE
constexpr std::uint32_t kInstanceDataBase = 32;           // c32 g_vInstanceData[200]
constexpr std::uint32_t kInstanceDataEnd = 232;
constexpr std::uint32_t kMaxBatch = 64;                    // instances read per instanced draw (the engine: <= 50)
constexpr std::uint64_t kOpaqueRecent = 600;               // frames: "the main view draws this range opaque"
// the TLAS is rebuilt every frame
constexpr VkBuildAccelerationStructureFlagsKHR kTlasFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;

// why a draw was not taken (logged once each)
enum Reason : std::uint32_t {
    kReasonDeclaration = 1,
    kReasonBaseVertex,
    kReasonIndexRange,
    kReasonVertexRange,
    kReasonReadFailed,
    kReasonAsCreate,
    kReasonScratch,
};

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::uint32_t word(const d3d9::VertexElement& e) { return static_cast<std::uint32_t>(e.offset) | (static_cast<std::uint32_t>(e.type) << 16); }

void split(VkDeviceAddress a, std::uint32_t (&out)[2]) {
    out[0] = static_cast<std::uint32_t>(a);
    out[1] = static_cast<std::uint32_t>(a >> 32);
}

bool createAs(vk::Context& ctx, VkAccelerationStructureTypeKHR type, VkDeviceSize size, vk::rt::AccelerationStructure& out,
              const char* name) {
    if (!out.storage.create(ctx, size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, 256, name)) return false;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.storage.buffer;
    ci.size = size;
    ci.type = type;
    if (const VkResult r = vkCreateAccelerationStructureKHR(ctx.device(), &ci, nullptr, &out.handle); r != VK_SUCCESS) {
        spdlog::error("rt: scene: vkCreateAccelerationStructureKHR ({}, {} bytes): {}", name, size, vk::resultString(r));
        out.destroy(ctx);
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.handle;
    out.address = vkGetAccelerationStructureDeviceAddressKHR(ctx.device(), &ai);
    return true;
}

}  // namespace

RtScene::RtScene(d3d9::Device& device) : dev_(device) {
    registry_.onAdded = [this](const rt::Registry::Instance& in) {
        if (Blas* b = findBlas(in.blas)) ++b->refs;
    };
    registry_.onRemoved = [this](const rt::Registry::Instance& in) {
        if (Blas* b = findBlas(in.blas); b != nullptr && b->refs > 0) --b->refs;
    };
}

RtScene::~RtScene() {
    consumers_.clear();
    deactivate();
}

bool RtScene::supported() const { return dev_.ctx().features().rayTracing.enabled; }

void RtScene::acquire(const void* consumer) {
    consumers_.insert(consumer);
    if (!active_ && supported() && !failed_) activate();
}

void RtScene::release(const void* consumer) {
    consumers_.erase(consumer);
    if (consumers_.empty() && active_) deactivate();
}

void RtScene::activate() {
    active_ = true;
    dev_.addDrawObserver(this);
    // before every other pass at AfterOpaque (they may trace it)
    passId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "rt.scene", [this](d3d9::PassContext& ctx) { record(ctx); }, this, -100);
    lastLog_ = std::chrono::steady_clock::now();
    iv_ = {};
    spdlog::info("rt: scene: on (TLAS cap {} instances, {} builds / {}k triangles per frame, BLAS budget {} MiB, radius {:.0f})",
                 limits_.maxInstances, limits_.buildsPerFrame, limits_.trianglesPerFrame / 1000, limits_.memoryBudget >> 20,
                 limits_.radius);
}

void RtScene::deactivate() {
    if (!active_) return;
    active_ = false;
    dev_.removeDrawObserver(this);
    if (passId_ != 0) dev_.removePass(passId_);
    passId_ = 0;
    logStats(true);
    registry_.clear();
    std::vector<std::uint64_t> ids;
    ids.reserve(blas_.size());
    for (const auto& [id, b] : blas_) ids.push_back(id);
    for (std::uint64_t id : ids) dropBlas(id, false);
    pending_.clear();
    destroyResources();
    dev_.setRtScene({});
    dev_.setStatus("rt.scene", {});
    spdlog::info("rt: scene: off (GPU memory released)");
}

// ---- draw observation -----------------------------------------------------------------------------------------------------

bool RtScene::observes(Kind kind, d3d9::scene::Phase phase) const {
    if (kind == Kind::Refracted) return phase.view == View::Main;  // learnt as water (excluded in every view)
    if (kind != Kind::StaticMesh && kind != Kind::InstancedMesh && kind != Kind::SpeedTree) return false;
    if (phase.view == View::Shadow) return (views_ & 2) != 0;
    // the main view's alpha passes too: their (blended) meshes are learnt as not opaque, the cascades draw them unblended
    return phase.view == View::Main && (views_ & 1) != 0 &&
           (phase.pass == Pass::Solid || phase.pass == Pass::DepthPrepass || phase.pass == Pass::Alpha || phase.pass == Pass::AlphaLate);
}

std::string RtScene::describe(std::uint64_t frame, std::uint32_t index) const {
    for (const auto& [f, list] : contents_) {
        if (f != frame) continue;
        if (index >= list.size()) return std::format("record {} beyond the {} instances", index, list.size());
        const Entry& e = list[index];
        std::string s = std::format("instance #{} at ({:.0f}, {:.0f}, {:.0f}), flags 0x{:x} (kind {}, views {}{}{}{})", e.instanceId,
                                    e.translation[0], e.translation[1], e.translation[2], e.flags,
                                    d3d9::draws::name(static_cast<Kind>((e.flags >> abi::kRtRecordKindShift) & 0xFF)),
                                    (e.flags >> abi::kRtRecordViewShift) & 0xFF, (e.flags & abi::kRtRecordAlphaTested) ? ", alpha-tested" : "",
                                    (e.flags & abi::kRtRecordInstanced) ? ", instanced" : "", (e.flags & abi::kRtRecordNoDepthWrite) ? ", no z write" : (e.flags & abi::kRtRecordDepthBias) ? ", depth bias" : "");
        const auto it = blas_.find(e.blas);
        if (it == blas_.end()) return s + ", BLAS gone";
        const Blas& b = it->second;
        const rt::BlasKey& k = b.key;
        return s + std::format("; BLAS #{}: vb {}#{} + {} stride {}, ib {}#{} first {} x {} triangles, base {}, bounds ({:.0f} {:.0f} {:.0f})..({:.0f} "
                               "{:.0f} {:.0f}); material {} entity {} op 0x{:x}",
                               b.id, k.vb, k.vbGen, k.vertexOffset, k.stride, k.ib, k.ibGen, k.first, k.primitives, k.baseVertex,
                               b.bounds.min[0], b.bounds.min[1], b.bounds.min[2], b.bounds.max[0], b.bounds.max[1], b.bounds.max[2],
                               b.material, b.entity, b.opDesc);
    }
    return std::format("frame {} not kept", frame);
}

void RtScene::setViews(std::uint8_t mask) {
    if (mask == views_) return;
    views_ = mask;
    registry_.clear();
    spdlog::info("rt: scene: sources {}", mask == 3 ? "main view + sun cascades" : mask == 1 ? "main view" : "sun cascades");
}

RtScene::Layout RtScene::layoutOf(const d3d9::VertexDecl* decl) {
    Layout l;
    if (decl == nullptr) return l;
    using shader::Usage;
    for (const d3d9::VertexElement& e : decl->elements()) {
        const auto usage = static_cast<Usage>(e.usage);
        if (usage == Usage::Position) {
            if (e.usageIndex == 0) {
                l.positionOk = e.stream == 0 && e.offset == 0 && e.type == kDeclFloat3;
            } else {
                l.unsupported = true;  // morph targets: positions + deltas
            }
            continue;
        }
        if (e.stream != 0) {
            if (usage == Usage::TexCoord && e.usageIndex == 4 && e.type == kDeclFloat1) {
                l.instanceStream = e.stream;
                l.instanceOffset = e.offset;
            }
            continue;
        }
        if (usage == Usage::Normal && e.usageIndex == 0) l.normal = word(e);
        else if (usage == Usage::TexCoord && e.usageIndex == 0) l.texcoord0 = word(e);
        else if (usage == Usage::TexCoord && e.usageIndex == 1) l.texcoord1 = word(e);
        else if (usage == Usage::TexCoord && e.usageIndex == 6) l.texcoord6 = word(e);
        else if (usage == Usage::Color && e.usageIndex == 0) l.color0 = word(e);
    }
    return l;
}

std::uint8_t RtScene::viewBit(View v) const { return v == View::Main ? 1 : v == View::Shadow ? 2 : 4; }

void RtScene::observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    if (!active_) return;
    const bool timed = dev.perfEnabled();
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct Timer {
        RtScene& s;
        bool on;
        std::chrono::steady_clock::time_point t0;
        ~Timer() {
            if (on) s.iv_.observeMs += msSince(t0);
        }
    } timer{*this, timed, t0};
    ++iv_.observed;
    const auto skip = [&] { ++iv_.skipped; };

    d3d9::Buffer* vb = q.vertexBuffer;
    d3d9::Buffer* ib = q.indexBuffer;
    if (q.primitiveType != 4 || vb == nullptr || ib == nullptr || q.primitives == 0 || q.engineVs == nullptr) return skip();
    // water reflects, it is not reflected: its meshes are learnt from the main view's refracted / water draws and left out
    // of every view (the sun cascades draw the sea too, untagged: StaticMesh there)
    // the same holds for meshes the main view draws alpha-blended (shore overlays, glass, ...)
    const bool mainBlended = q.phase.view == View::Main && q.blending;
    const bool water = q.kind == Kind::Refracted || (q.tag != nullptr && q.tag->water != d3d9::draws::Water::None);
    if (water || mainBlended) {
        learnExcluded(q, water);
        return skip();
    }
    if (q.phase.view == View::Main && (q.phase.pass == Pass::Alpha || q.phase.pass == Pass::AlphaLate)) return skip();
    if (q.tag != nullptr && q.tag->material != nullptr && waterMaterials_.contains(q.tag->material)) return skip();
    if (q.blending) return skip();                                                     // alpha-blended: not P1
    const bool speedTree = q.kind == Kind::SpeedTree;
    if (speedTree && (q.tag == nullptr || q.tag->opDesc > 1)) return skip();           // leaves (cards / meshes): P4
    const d3d9::ShaderSemantics& sem = q.engineVs->semantics;
    using S = d3d9::ShaderSemantics;
    if ((sem.flags & S::Material) == 0 || (sem.flags & (S::Skinned | S::Vegetation | S::Leaves)) != 0) return skip();
    const auto& instanceData = sem[d3d9::Semantic::InstanceData];
    if (!instanceData.present() || static_cast<std::uint32_t>(instanceData.reg) != kInstanceDataBase) return skip();
    if (sem[d3d9::Semantic::InvView].present()) return skip();  // screen-aligned billboard meshes: camera-dependent
    if (q.instanced != ((sem.flags & S::Instanced) != 0)) return skip();
    if (vb->ringCopied() || ib->ringCopied()) return skip();   // dynamic geometry: one frame only
    if (q.vertexStride < 12 || q.vertexStride % 4 != 0 || q.vertexOffset % 4 != 0) return skip();
    const Layout layout = layoutOf(q.decl);
    if (!layout.positionOk || layout.unsupported || (q.instanced && layout.instanceStream < 0)) {
        if (declLogged_.size() < 64 && declLogged_.insert(q.decl).second) {
            std::string d;
            if (q.decl != nullptr) {
                for (const auto& e : q.decl->elements()) d += std::format(" {}{}@{}+{}:t{}", e.usage, e.usageIndex, e.stream, e.offset, e.type);
            }
            spdlog::info("rt: scene: skipping draws of declaration [{} ] ({} {}, stride {}): no POSITION FLOAT3 at offset 0 of stream 0, "
                         "morph targets or no instance stream",
                         d, d3d9::draws::name(q.kind), q.instanced ? "instanced" : "plain", q.vertexStride);
        }
        return skip();
    }
    // the buffers' current GPU copies (the draw binds them right after): a new generation drops what was built from the old
    vb->gpu();
    ib->gpu();
    const VkDeviceAddress vbAddress = vb->deviceAddress(), ibAddress = ib->deviceAddress();
    if (vbAddress == 0 || ibAddress == 0) return skip();
    const d3d9::ResourceRef vr = vb->ref(), ir = ib->ref();
    rt::BlasKey key;
    key.vb = vr.id;
    key.vbGen = vr.generation;
    key.ib = ir.id;
    key.ibGen = ir.generation;
    key.vertexOffset = q.vertexOffset;
    key.stride = q.vertexStride;
    key.first = q.first;
    key.primitives = q.primitives;
    key.baseVertex = q.baseVertex;
    key.index32 = ib->index32() ? 1u : 0u;

    if (waterKeys_.contains(key)) return skip();
    // a range the main view draws only blended (the cascades draw it unblended) is not part of the scene
    const bool mainOpaque = q.phase.view == View::Main;
    if (!mainOpaque && !blendedKeys_.empty()) {
        if (const auto it = blendedKeys_.find(key); it != blendedKeys_.end() && it->second + kOpaqueRecent >= dev.frameValue()) {
            const auto bk = blasByKey_.find(key);
            const Blas* known = bk != blasByKey_.end() ? findBlas(bk->second) : nullptr;
            if (known == nullptr || known->opaqueMainFrame + kOpaqueRecent < dev.frameValue()) return skip();
        }
    }
    const std::uint64_t frame = dev.frameValue();
    const double now = dev.secondsSinceCreate();
    Blas* b = nullptr;
    if (const auto it = blasByKey_.find(key); it != blasByKey_.end()) {
        b = findBlas(it->second);
        if (b != nullptr) {
            // a later declaration may name more attributes (the shadow pass's can be shorter)
            Layout& l = b->layout;
            for (auto [dst, src] : {std::pair{&l.normal, layout.normal}, {&l.texcoord0, layout.texcoord0}, {&l.texcoord1, layout.texcoord1},
                                    {&l.texcoord6, layout.texcoord6}, {&l.color0, layout.color0}}) {
                if (*dst == abi::kRtAttribAbsent) *dst = src;
            }
        }
    }
    if (b == nullptr) {
        const std::uint64_t id = nextBlasId_++;
        Blas& nb = blas_[id];
        nb.id = id;
        nb.key = key;
        nb.vb = vb;
        nb.ib = ib;
        nb.vbAddress = vbAddress;
        nb.ibAddress = ibAddress;
        nb.layout = layout;
        nb.kind = static_cast<std::uint32_t>(q.kind);
        if (q.tag != nullptr) {
            nb.material = q.tag->material;
            nb.entity = q.tag->entity;
            nb.opDesc = q.tag->opDesc;
        }
        blasByKey_[key] = id;
        blasByBuffer_.emplace(vr.id, id);
        if (ir.id != vr.id) blasByBuffer_.emplace(ir.id, id);
        pending_.push_back(id);
        b = &nb;
    }
    if (b->state == BlasState::Failed) return skip();
    b->lastUsedFrame = frame;
    b->lastUsedTime = now;
    if (mainOpaque) b->opaqueMainFrame = frame;

    // beyond the radius (bounds known once built): not taken (the registry would age it out again at once)
    const d3d9::scene::CameraBlock& camBlock = dev.camera().valid ? dev.camera() : dev.lastCamera();
    const auto outside = [&](const rt::Rows& rows) {
        if (!camBlock.valid || limits_.radius <= 0.0f || !b->bounds.valid()) return false;
        const rt::Sphere s = rt::worldSphere(b->bounds, rows);
        const float dx = s.c[0] - camBlock.position[0], dy = s.c[1] - camBlock.position[1], dz = s.c[2] - camBlock.position[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz) - s.r > limits_.radius;
    };
    const bool alphaTested = q.alphaTest || (speedTree && q.tag->opDesc == 1);
    const std::uint32_t flags = (key.index32 != 0 ? abi::kRtRecordIndex32 : 0u) | (alphaTested ? abi::kRtRecordAlphaTested : 0u) |
                                (q.depthBias && q.phase.view == View::Main ? abi::kRtRecordDepthBias : 0u) |
                                (speedTree ? abi::kRtRecordSpeedTree : 0u) | (q.instanced ? abi::kRtRecordInstanced : 0u) |
                                (!q.depthWrite && q.phase.view == View::Main && q.phase.pass == Pass::Solid ? abi::kRtRecordNoDepthWrite : 0u) |
                                (static_cast<std::uint32_t>(viewBit(q.phase.view)) << abi::kRtRecordViewShift);
    // the instance's data word: the alpha test (func | ref << 8, 0 = none) and the draw kind << 16 (the latest sighting's;
    // flags are OR-ed over sightings, a kind must not be)
    const std::uint32_t alphaWord = (q.alphaTest ? (q.alphaFunc | (q.alphaRef << 8)) : 0u) | (static_cast<std::uint32_t>(q.kind) << 16);
    const std::uint8_t view = viewBit(q.phase.view);
    const auto rowsAt = [&](std::uint32_t reg, rt::Rows& rows) {
        for (std::uint32_t r = 0; r < 3; ++r) std::memcpy(&rows[r * 4], dev.vsConstant(reg + r), sizeof(float) * 4);
        return rt::finite(rows);
    };
    if (!q.instanced) {
        rt::Rows rows;
        if (!rowsAt(kInstanceDataBase + 1, rows) || outside(rows)) return skip();
        const std::uint64_t owner = q.tag != nullptr ? rt::ownerKey(q.tag->entity, q.tag->opDesc) : 0;
        registry_.see(rt::instanceKey(b->id, rows), b->id, rows, owner, flags, alphaWord, view, frame, now);
        return;
    }
    // instanced batch: instance i reads g_vInstanceData[floor(4 v) + 1 ..] with v = its TEXCOORD4 (the instance stream)
    const std::uint32_t n = std::min(q.instances, kMaxBatch);
    float index[kMaxBatch];
    bool fromStream = false;
    const d3d9::Device::StreamSource src = dev.streamSource(static_cast<std::uint32_t>(layout.instanceStream));
    if (src.buffer != nullptr && src.stride >= 4) {
        std::uint8_t bytes[kMaxBatch * 64];
        const std::uint32_t span = (n - 1) * src.stride + layout.instanceOffset + 4;
        if (src.stride <= 64 && span <= sizeof(bytes) && src.buffer->readCpu(src.offset, span, bytes)) {
            for (std::uint32_t i = 0; i < n; ++i) std::memcpy(&index[i], bytes + i * src.stride + layout.instanceOffset, 4);
            fromStream = true;
        }
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const float v = fromStream ? index[i] : static_cast<float>(i);
        if (fromStream && v != static_cast<float>(i)) ++indexDeviations_;
        if (!std::isfinite(v) || v < 0.0f || v > 64.0f) continue;
        const std::uint32_t reg = kInstanceDataBase + static_cast<std::uint32_t>(std::floor(4.0f * v + 0.5f)) + 1;
        if (reg + 2 >= kInstanceDataEnd) continue;
        rt::Rows rows;
        if (!rowsAt(reg, rows) || outside(rows)) continue;
        registry_.see(rt::instanceKey(b->id, rows), b->id, rows, 0, flags, alphaWord, view, frame, now);
    }
    if (!instanceIndexChecked_ && fromStream && iv_.observed > 2000) {
        instanceIndexChecked_ = true;
        spdlog::info("rt: scene: instance stream check: {}", indexDeviations_ == 0 ? "TEXCOORD4 = 0, 1, 2, ... in every batch so far"
                                                                                   : std::format("{} instances deviate from 0, 1, 2, ...", indexDeviations_));
    }
}

void RtScene::learnExcluded(const d3d9::ReplaceQuery& q, bool water) {
    // water by material (all of its meshes) and draw range; alpha-blended meshes by draw range only, and only until the main
    // view draws that range opaque (objects fading in are drawn blended for a while)
    if (water && q.tag != nullptr && q.tag->material != nullptr && waterMaterials_.insert(q.tag->material).second) {
        spdlog::info("rt: scene: water material {} '{}' left out of the RT scene", q.tag->material,
                     q.tag->materialName != nullptr ? q.tag->materialName : "?");
    }
    if (q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.vertexBuffer->ringCopied() || q.indexBuffer->ringCopied()) return;
    const d3d9::ResourceRef vr = q.vertexBuffer->ref(), ir = q.indexBuffer->ref();
    rt::BlasKey key;
    key.vb = vr.id;
    key.vbGen = vr.generation;
    key.ib = ir.id;
    key.ibGen = ir.generation;
    key.vertexOffset = q.vertexOffset;
    key.stride = q.vertexStride;
    key.first = q.first;
    key.primitives = q.primitives;
    key.baseVertex = q.baseVertex;
    key.index32 = q.indexBuffer->index32() ? 1u : 0u;
    if (water) {
        if (!waterKeys_.insert(key).second) return;
        if (const auto it = blasByKey_.find(key); it != blasByKey_.end()) dropBlas(it->second, false);
        return;
    }
    // blended: excluded unless the main view also draws the range opaque (multi-pass materials: an opaque base pass plus
    // blended layers on the same geometry, the terrain's; objects fading in)
    const std::uint64_t frame = dev_.frameValue();
    blendedKeys_[key] = frame;
    if (const auto it = blasByKey_.find(key); it != blasByKey_.end()) {
        const Blas* b = findBlas(it->second);
        if (b != nullptr && b->opaqueMainFrame + kOpaqueRecent < frame) dropBlas(it->second, false);
    }
}

void RtScene::bufferChanged(std::uint64_t bufferId) {
    if (!active_) return;
    std::erase_if(waterKeys_, [&](const rt::BlasKey& k) { return k.vb == bufferId || k.ib == bufferId; });
    std::erase_if(blendedKeys_, [&](const auto& e) { return e.first.vb == bufferId || e.first.ib == bufferId; });
    const auto range = blasByBuffer_.equal_range(bufferId);
    if (range.first == range.second) return;
    std::vector<std::uint64_t> ids;
    for (auto it = range.first; it != range.second; ++it) ids.push_back(it->second);
    for (std::uint64_t id : ids) dropBlas(id, false);
}

RtScene::Blas* RtScene::findBlas(std::uint64_t id) {
    const auto it = blas_.find(id);
    return it != blas_.end() ? &it->second : nullptr;
}

void RtScene::dropBlas(std::uint64_t id, bool countEvicted) {
    Blas* b = findBlas(id);
    if (b == nullptr) return;
    registry_.removeBlas(id);
    blasByKey_.erase(b->key);
    for (std::uint64_t buf : {b->key.vb, b->key.ib}) {
        const auto range = blasByBuffer_.equal_range(buf);
        for (auto it = range.first; it != range.second;) {
            it = it->second == id ? blasByBuffer_.erase(it) : std::next(it);
        }
    }
    if (b->as.handle != VK_NULL_HANDLE || b->as.storage.buffer != VK_NULL_HANDLE) {
        blasBytes_ -= std::min(blasBytes_, static_cast<std::uint64_t>(b->as.storage.size));
        vk::Context* ctx = &dev_.ctx();
        vk::rt::AccelerationStructure old = b->as;
        dev_.frames().retire([ctx, old]() mutable { old.destroy(*ctx); });
    }
    if (countEvicted) ++iv_.evicted;
    blas_.erase(id);
}

// ---- GPU resources --------------------------------------------------------------------------------------------------------

bool RtScene::ensureScratch(VkDeviceSize bytes) {
    if (scratch_.buffer != VK_NULL_HANDLE && scratch_.size >= bytes) return true;
    vk::Context& ctx = dev_.ctx();
    if (scratch_.buffer != VK_NULL_HANDLE) {
        vk::Context* c = &ctx;
        vk::rt::AddressBuffer old = scratch_;
        dev_.frames().retire([c, old]() mutable { old.destroy(*c); });
        scratch_ = {};
    }
    const VkDeviceSize size = std::max<VkDeviceSize>(bytes, 32ull << 20);
    return scratch_.create(ctx, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratchAlign_, "rt scene scratch");
}

bool RtScene::ensureResources() {
    if (tlas_[0].as.handle != VK_NULL_HANDLE) return true;
    vk::Context& ctx = dev_.ctx();
    scratchAlign_ = std::max<VkDeviceSize>(ctx.features().rayTracing.minScratchOffsetAlignment, 16);
    // the TLAS of each frame slot, sized for the cap
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = kTlasFlags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const std::uint32_t cap = limits_.maxInstances;
    vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &cap, &sizes);
    bool ok = true;
    for (Tlas& t : tlas_) {
        ok = ok && createAs(ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize, t.as, "rt scene tlas");
        t.capacity = cap;
    }
    tlasScratchSize_ = (sizes.buildScratchSize + scratchAlign_ - 1) & ~(scratchAlign_ - 1);
    ok = ok && ensureScratch(tlasScratchSize_);
    // compaction-size queries of each slot's builds
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    qi.queryCount = kQueriesPerSlot * vk::Frames::kInFlight;
    ok = ok && vkCreateQueryPool(ctx.device(), &qi, nullptr, &compactPool_) == VK_SUCCESS;
    if (ok && dev_.perfEnabled() && ctx.features().timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo ti{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ti.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ti.queryCount = 2 * vk::Frames::kInFlight;
        if (vkCreateQueryPool(ctx.device(), &ti, nullptr, &timePool_) != VK_SUCCESS) timePool_ = VK_NULL_HANDLE;
    }
    for (auto& w : timeWritten_) w = false;
    for (auto& c : compactions_) c.clear();
    if (!ok) {
        spdlog::error("rt: scene: GPU resources failed, the RT scene stays off this session");
        failed_ = true;
        destroyResources();
        return false;
    }
    spdlog::info("rt: scene: TLAS {} instances = {:.1f} MiB x {}, build scratch {:.1f} MiB", cap,
                 static_cast<double>(sizes.accelerationStructureSize) / 1048576.0, vk::Frames::kInFlight,
                 static_cast<double>(scratch_.size) / 1048576.0);
    return true;
}

void RtScene::destroyResources() {
    vk::Context* ctx = &dev_.ctx();
    std::vector<vk::rt::AccelerationStructure> tl;
    for (Tlas& t : tlas_) {
        tl.push_back(t.as);
        t = {};
    }
    vk::rt::AddressBuffer scratch = scratch_;
    scratch_ = {};
    const VkQueryPool cp = compactPool_, tp = timePool_;
    compactPool_ = timePool_ = VK_NULL_HANDLE;
    for (auto& c : compactions_) c.clear();
    dev_.frames().retire([ctx, tl, scratch, cp, tp]() mutable {
        for (auto& t : tl) t.destroy(*ctx);
        scratch.destroy(*ctx);
        if (cp != VK_NULL_HANDLE) vkDestroyQueryPool(ctx->device(), cp, nullptr);
        if (tp != VK_NULL_HANDLE) vkDestroyQueryPool(ctx->device(), tp, nullptr);
    });
}

void RtScene::barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

// ---- builds ---------------------------------------------------------------------------------------------------------------

bool RtScene::prepareInputs(Blas& b) {
    const auto fail = [&](Reason r, const std::string& what) {
        if (reasonsLogged_.insert(r).second) spdlog::warn("rt: scene: BLAS skipped (first of its kind): {}", what);
        return false;
    };
    const rt::BlasKey& k = b.key;
    const std::uint32_t isize = k.index32 != 0 ? 4u : 2u;
    const std::uint32_t count = k.primitives * 3;
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(count) * isize);
    if (!b.ib->readCpu(k.first * isize, count * isize, idx.data())) {
        return fail(kReasonIndexRange, std::format("index range {} + {} outside the index buffer ({} bytes)", k.first, count, b.ib->size()));
    }
    std::uint32_t lo = UINT32_MAX, hi = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t v;
        if (isize == 4) {
            std::memcpy(&v, idx.data() + i * 4, 4);
        } else {
            std::uint16_t s;
            std::memcpy(&s, idx.data() + i * 2, 2);
            v = s;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (k.baseVertex < 0) {
        // vertex 0 of the BLAS input (stream offset + base vertex) would lie before the buffer
        return fail(kReasonBaseVertex, std::format("negative base vertex {} (min index {})", k.baseVertex, lo));
    }
    const std::uint64_t first = k.vertexOffset + (static_cast<std::uint64_t>(k.baseVertex) + lo) * k.stride;
    const std::uint64_t bytes = static_cast<std::uint64_t>(hi - lo) * k.stride + 12;
    if (first + bytes > b.vb->size()) {
        return fail(kReasonVertexRange, std::format("vertices {}..{} (base {}, stride {}) beyond the vertex buffer ({} bytes)", lo, hi,
                                                    k.baseVertex, k.stride, b.vb->size()));
    }
    std::vector<std::uint8_t> verts(static_cast<std::size_t>(bytes));
    if (!b.vb->readCpu(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bytes), verts.data())) {
        return fail(kReasonReadFailed, "vertex read failed");
    }
    rt::Aabb box;
    for (std::uint64_t off = 0; off + 12 <= bytes; off += k.stride) {
        float p[3];
        std::memcpy(p, verts.data() + off, 12);
        if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) box.add(p);
    }
    b.bounds = box;
    b.maxVertex = hi;
    return true;
}

void RtScene::processCompactions(VkCommandBuffer cmd, std::uint32_t slot) {
    auto& list = compactions_[slot];
    if (list.empty()) return;
    vk::Context& ctx = dev_.ctx();
    std::vector<std::uint64_t> results(list.size() * 2, 0);
    vkGetQueryPoolResults(ctx.device(), compactPool_, slot * kQueriesPerSlot, static_cast<std::uint32_t>(list.size()),
                          results.size() * sizeof(std::uint64_t), results.data(), 2 * sizeof(std::uint64_t),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Compaction& c = list[i];
        if (results[i * 2 + 1] == 0) continue;  // not available (should not happen: the slot's earlier submission completed)
        Blas* b = findBlas(c.blas);
        if (b == nullptr || b->serial != c.serial || b->state != BlasState::Built || b->compacted) continue;
        b->compacted = true;
        const VkDeviceSize size = results[i * 2];
        if (size == 0 || static_cast<double>(size) > 0.9 * static_cast<double>(b->as.storage.size)) continue;
        vk::rt::AccelerationStructure dst;
        if (!createAs(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, size, dst, "rt scene blas (compacted)")) continue;
        VkCopyAccelerationStructureInfoKHR ci{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        ci.src = b->as.handle;
        ci.dst = dst.handle;
        ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(cmd, &ci);
        vk::Context* pc = &ctx;
        vk::rt::AccelerationStructure old = b->as;
        dev_.frames().retire([pc, old]() mutable { old.destroy(*pc); });
        iv_.compactSaved += old.storage.size - dst.storage.size;
        blasBytes_ = blasBytes_ - std::min(blasBytes_, static_cast<std::uint64_t>(old.storage.size)) + dst.storage.size;
        b->as = dst;
        ++iv_.compacted;
    }
    list.clear();
}

void RtScene::buildPending(VkCommandBuffer cmd, std::uint32_t slot) {
    if (pending_.empty()) return;
    vk::Context& ctx = dev_.ctx();
    rt::BuildBudget budget(limits_.buildsPerFrame, limits_.trianglesPerFrame);
    const std::size_t maxBuilds = std::min<std::size_t>(limits_.buildsPerFrame, kQueriesPerSlot);
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    geoms.reserve(maxBuilds);
    infos.reserve(maxBuilds);
    ranges.reserve(maxBuilds);
    std::vector<std::uint64_t> built;
    VkDeviceSize scratchUsed = 0;
    const VkPipelineStageFlags2 asStages =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        (ctx.features().rayTracing.maintenance1 ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR : 0);
    const auto flush = [&] {
        if (infos.empty()) return;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pr(ranges.size());
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            infos[i].pGeometries = &geoms[i];
            pr[i] = &ranges[i];
        }
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<std::uint32_t>(infos.size()), infos.data(), pr.data());
        geoms.clear();
        infos.clear();
        ranges.clear();
    };
    while (!pending_.empty() && built.size() < maxBuilds) {
        Blas* b = findBlas(pending_.front());
        if (b == nullptr || b->state != BlasState::Pending) {
            pending_.pop_front();
            continue;
        }
        if (!budget.take(b->key.primitives)) break;
        pending_.pop_front();
        const auto c0 = std::chrono::steady_clock::now();
        const rt::BlasKey& k = b->key;
        if (!prepareInputs(*b)) {
            b->state = BlasState::Failed;
            ++iv_.failed;
            registry_.removeBlas(b->id);
            continue;
        }
        const std::uint32_t isize = k.index32 != 0 ? 4u : 2u;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;  // alpha-tested instances: FORCE_NO_OPAQUE on the instance
        auto& tri = g.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = b->vbAddress + k.vertexOffset + static_cast<VkDeviceSize>(k.baseVertex) * k.stride;
        tri.vertexStride = k.stride;
        tri.maxVertex = b->maxVertex;
        tri.indexType = k.index32 != 0 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        tri.indexData.deviceAddress = b->ibAddress + static_cast<VkDeviceSize>(k.first) * isize;
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &k.primitives, &sizes);
        const VkDeviceSize need = (std::max<VkDeviceSize>(sizes.buildScratchSize, 4) + scratchAlign_ - 1) & ~(scratchAlign_ - 1);
        if (scratchUsed + need > scratch_.size) {
            // the scratch is full: record what we have, wait for it (scratch reuse), continue at its start
            flush();
            barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
            scratchUsed = 0;
            if (need > scratch_.size && !ensureScratch(need)) {
                if (reasonsLogged_.insert(kReasonScratch).second) spdlog::warn("rt: scene: build scratch of {} bytes failed", need);
                b->state = BlasState::Failed;
                ++iv_.failed;
                registry_.removeBlas(b->id);
                continue;
            }
        }
        if (!createAs(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, b->as, "rt scene blas")) {
            if (reasonsLogged_.insert(kReasonAsCreate).second) spdlog::warn("rt: scene: BLAS of {} bytes could not be created", sizes.accelerationStructureSize);
            b->state = BlasState::Failed;
            ++iv_.failed;
            registry_.removeBlas(b->id);
            continue;
        }
        info.dstAccelerationStructure = b->as.handle;
        info.scratchData.deviceAddress = scratch_.address + scratchUsed;
        scratchUsed += need;
        geoms.push_back(g);
        infos.push_back(info);
        ranges.push_back({k.primitives, 0, 0, 0});
        b->state = BlasState::Built;
        ++b->serial;
        blasBytes_ += b->as.storage.size;
        built.push_back(b->id);
        ++iv_.built;
        iv_.builtTriangles += k.primitives;
        iv_.buildCpuMs += msSince(c0);
    }
    flush();
    if (built.empty()) return;
    // the builds -> their compacted-size queries (and the TLAS build after them)
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, asStages,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    std::vector<VkAccelerationStructureKHR> handles;
    auto& list = compactions_[slot];
    list.clear();
    for (std::uint64_t id : built) {
        const Blas* b = findBlas(id);
        if (b == nullptr) continue;
        list.push_back({id, b->serial, static_cast<std::uint32_t>(handles.size())});
        handles.push_back(b->as.handle);
    }
    if (handles.empty()) return;
    vkCmdResetQueryPool(cmd, compactPool_, slot * kQueriesPerSlot, static_cast<std::uint32_t>(handles.size()));
    vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, static_cast<std::uint32_t>(handles.size()), handles.data(),
                                                  VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, compactPool_,
                                                  slot * kQueriesPerSlot);
}

void RtScene::evictOverBudget() {
    const std::uint64_t frame = dev_.frameValue();
    const double now = dev_.secondsSinceCreate();
    // without instances for long: dropped even under the budget
    std::vector<std::uint64_t> stale;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> lru;  // last used frame, id: unused built BLASes
    for (const auto& [id, b] : blas_) {
        if (b.refs != 0) continue;
        if (frame > b.lastUsedFrame + limits_.blasUnusedFrames && now > b.lastUsedTime + limits_.blasUnusedSeconds) {
            stale.push_back(id);
        } else if (b.state == BlasState::Built) {
            lru.emplace_back(b.lastUsedFrame, id);
        }
    }
    for (std::uint64_t id : stale) dropBlas(id, true);
    // over the budget: the least recently used unused ones until 85 % of it
    if (blasBytes_ <= limits_.memoryBudget) return;
    std::sort(lru.begin(), lru.end());
    const std::uint64_t target = limits_.memoryBudget / 100 * 85;
    for (const auto& [last, id] : lru) {
        if (blasBytes_ <= target) break;
        dropBlas(id, true);
    }
    if (blasBytes_ > limits_.memoryBudget && reasonsLogged_.insert(0x100).second) {
        spdlog::warn("rt: scene: BLAS memory {:.0f} MiB over the budget of {} MiB with every BLAS in use",
                     static_cast<double>(blasBytes_) / 1048576.0, limits_.memoryBudget >> 20);
    }
}

// ---- the pass ----------------------------------------------------------------------------------------------------------------

void RtScene::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !active_ || failed_) return;
    if (!ensureResources()) return;
    const auto t0 = std::chrono::steady_clock::now();
    vk::Context& vctx = dev_.ctx();
    vk::Frames& frames = dev_.frames();
    const std::uint32_t slot = frames.currentSlot();
    const std::uint64_t frame = dev_.frameValue();
    const double now = dev_.secondsSinceCreate();
    VkCommandBuffer cmd = ctx.cmd;
    const VkPipelineStageFlags2 asStages =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        (vctx.features().rayTracing.maintenance1 ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR : 0);
    const VkPipelineStageFlags2 rayStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    // GPU time of the pass ([debug] perf): this slot's previous one has completed (Frames::begin waited for it)
    if (timePool_ != VK_NULL_HANDLE) {
        if (timeWritten_[slot]) {
            std::uint64_t t[2]{};
            if (vkGetQueryPoolResults(vctx.device(), timePool_, slot * 2, 2, sizeof(t), t, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS && t[1] >= t[0]) {
                iv_.gpuMs += static_cast<double>(t[1] - t[0]) * vctx.features().timestampPeriod * 1e-6;
                ++iv_.gpuSamples;
            }
        }
        vkCmdResetQueryPool(cmd, timePool_, slot * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timePool_, slot * 2);
    }

    // earlier submissions' builds (the shared scratch) and ray queries of this slot's TLAS before this frame's writes
    barrier(cmd, asStages | rayStages, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, asStages,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    // the camera of this frame's main view
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    const float camera[3] = {cam.position[0], cam.position[1], cam.position[2]};
    registry_.age(camera, frame, now, limits_, [this](std::uint64_t id) -> const rt::Aabb* {
        const Blas* b = findBlas(id);
        return b != nullptr && b->bounds.valid() ? &b->bounds : nullptr;
    });
    evictOverBudget();
    if (frame % 300 == 0) std::erase_if(blendedKeys_, [&](const auto& e) { return e.second + kOpaqueRecent < frame; });
    processCompactions(cmd, slot);
    buildPending(cmd, slot);
    // BLAS builds / compaction copies -> the TLAS build
    barrier(cmd, asStages, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    // the instances and their geometry records (one geometry per BLAS: record index = instanceCustomIndex)
    const auto& list = registry_.instances();
    const std::vector<std::uint32_t> chosen = rt::selectForTlas(list, camera, tlas_[slot].capacity, [this](const rt::Registry::Instance& in) {
        const Blas* b = findBlas(in.blas);
        // an object the cascades draw at another level of detail than the main view: the main view's
        return b != nullptr && b->state == BlasState::Built && !registry_.supersededBy(in, viewBit(View::Main));
    });
    const std::uint32_t n = static_cast<std::uint32_t>(chosen.size());
    const vk::RingAlloc ia = frames.allocate(std::max<VkDeviceSize>(n, 1) * sizeof(VkAccelerationStructureInstanceKHR), 16);
    const vk::RingAlloc ra = frames.allocate(std::max<VkDeviceSize>(n, 1) * sizeof(abi::RtGeometryRecord), 16);
    if (ia.ptr == nullptr || ra.ptr == nullptr) {
        dev_.setRtScene({});
        return;
    }
    auto* insts = static_cast<VkAccelerationStructureInstanceKHR*>(ia.ptr);
    auto* recs = static_cast<abi::RtGeometryRecord*>(ra.ptr);
    std::uint64_t triangles = 0;
    // the contents for diagnostics (rt.debug's probes) while a debug view is on
    std::vector<Entry>* content = nullptr;
    if (keepContents_) {
        if (contents_.size() >= 4) contents_.pop_front();
        contents_.emplace_back(frame, std::vector<Entry>{});
        content = &contents_.back().second;
        content->reserve(n);
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        const rt::Registry::Instance& in = list[chosen[k]];
        const Blas& b = *findBlas(in.blas);
        const bool alpha = (in.flags & abi::kRtRecordAlphaTested) != 0;
        VkAccelerationStructureInstanceKHR vi{};
        std::memcpy(vi.transform.matrix, in.rows.data(), sizeof(vi.transform.matrix));
        vi.instanceCustomIndex = k;
        vi.mask = alpha ? abi::kRtMaskAlphaTested : abi::kRtMaskOpaque;
        vi.instanceShaderBindingTableRecordOffset = 0;
        vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                   (alpha ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
        vi.accelerationStructureReference = b.as.address;
        insts[k] = vi;
        abi::RtGeometryRecord r{};
        const rt::BlasKey& key = b.key;
        split(b.vbAddress + key.vertexOffset + static_cast<VkDeviceSize>(key.baseVertex) * key.stride, r.vertices);
        split(b.ibAddress + static_cast<VkDeviceSize>(key.first) * (key.index32 != 0 ? 4u : 2u), r.indices);
        r.stride = key.stride;
        r.flags = in.flags | (((in.data >> 16) & 0xFFu) << abi::kRtRecordKindShift);
        r.normal = b.layout.normal;
        r.texcoord0 = b.layout.texcoord0;
        r.texcoord1 = b.layout.texcoord1;
        r.texcoord6 = b.layout.texcoord6;
        r.color0 = b.layout.color0;
        r.instanceId = in.id;
        r.alpha = (in.data & 0xFFFFu) != 0 ? (in.data & 0xFFFFu) : 8u;  // D3DCMP_ALWAYS
        r.uvMap[0][0] = 1.0f;                    // identity uv map until P2 fills the material
        r.uvMap[1][1] = 1.0f;
        recs[k] = r;
        triangles += key.primitives;
        if (content != nullptr) content->push_back({in.blas, in.id, r.flags, {in.rows[3], in.rows[7], in.rows[11]}});
    }

    Tlas& t = tlas_[slot];
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    g.geometry.instances.data.deviceAddress = ia.address;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = kTlasFlags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    info.dstAccelerationStructure = t.as.handle;
    info.scratchData.deviceAddress = scratch_.address;
    const VkAccelerationStructureBuildRangeInfoKHR range{n, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &pr);
    // the TLAS -> ray queries in fragment / compute shaders of this submission
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, rayStages,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    if (timePool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timePool_, slot * 2 + 1);
        timeWritten_[slot] = true;
    }
    dev_.setRtScene({t.as.address, ra.address, n, n, frame});

    lastInstances_ = n;
    lastTriangles_ = triangles;
    ++iv_.frames;
    const double passMs = msSince(t0);
    iv_.passCpuMs += passMs;
    if (passMs > 50.0) {
        spdlog::info("rt: scene: slow pass: {:.1f} ms CPU ({} BLAS built this interval, {} instances; the driver's first acceleration "
                     "structure builds compile its build kernels)",
                     passMs, iv_.built, n);
    }
    logStats(false);
}

void RtScene::logStats(bool force) {
    const auto now = std::chrono::steady_clock::now();
    const bool due = now - lastLog_ >= std::chrono::seconds(5);
    std::size_t built = 0, pendingCount = 0, failed = 0;
    for (const auto& [id, b] : blas_) {
        built += b.state == BlasState::Built ? 1 : 0;
        pendingCount += b.state == BlasState::Pending ? 1 : 0;
        failed += b.state == BlasState::Failed ? 1 : 0;
    }
    const double tlasMiB = static_cast<double>(tlas_[0].as.storage.size * vk::Frames::kInFlight) / 1048576.0;
    const double blasMiB = static_cast<double>(blasBytes_) / 1048576.0;
    const double scratchMiB = static_cast<double>(scratch_.size) / 1048576.0;
    if (iv_.frames % 30 == 1 || due || force) {
        dev_.setStatus("rt.scene", std::format("RT scene: {} instances, {} BLAS, {:.1f}M triangles, {:.0f} MiB", lastInstances_, built,
                                               static_cast<double>(lastTriangles_) / 1e6, blasMiB + tlasMiB + scratchMiB));
    }
    if (!due && !force) return;
    const double f = std::max<double>(1.0, static_cast<double>(iv_.frames));
    const auto& c = registry_.counters();
    spdlog::info("rt: scene: instances {} in the TLAS (registry {}: added {}, replaced {}, aged {}, far {}, capped {}, dropped {}), "
                 "blas {} (+built {} / {}k triangles, pending {}, failed {}, compacted {} saving {:.1f} MiB, evicted {}), triangles {}, "
                 "AS memory {:.1f} MiB (BLAS {:.1f}, TLAS {:.1f}, scratch {:.1f}); per frame: observe {:.3f} ms ({:.0f} draws, {:.0f} "
                 "skipped), build CPU {:.3f} ms, pass CPU {:.3f} ms, GPU {}",
                 lastInstances_, registry_.size(), c.added, c.replaced, c.aged, c.tooFar, c.capped, c.dropped, built, iv_.built,
                 iv_.builtTriangles / 1000, pendingCount, failed, iv_.compacted, static_cast<double>(iv_.compactSaved) / 1048576.0, iv_.evicted,
                 lastTriangles_, blasMiB + tlasMiB + scratchMiB, blasMiB, tlasMiB, scratchMiB, iv_.observeMs / f,
                 static_cast<double>(iv_.observed) / f, static_cast<double>(iv_.skipped) / f, iv_.buildCpuMs / f, iv_.passCpuMs / f,
                 iv_.gpuSamples > 0 ? std::format("{:.3f} ms", iv_.gpuMs / static_cast<double>(iv_.gpuSamples)) : std::string("n/a ([debug] perf)"));
    iv_ = {};
    lastLog_ = now;
}

}  // namespace rvk::enh
