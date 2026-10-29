#include "enh/rt_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "engine/materials.h"
#include "enh/material_variant.h"
#include "shader/embedded.h"
#include "engine/memread.h"
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
constexpr std::uint64_t kChurnWindow = 90;   // frames: a range with kChurnLimit new instances in this many is volatile (rt-shadows.md 5)
constexpr std::uint32_t kChurnLimit = 48;
constexpr std::uint64_t kVolatileKeep = 3;   // frames a volatile range's instance stays in the TLAS after its last sighting
constexpr std::uint32_t kMaxAnimatedTrees = 160;  // SpeedTree draws captured per frame (their captures take 32 bytes per vertex of the frame's ring)
constexpr std::uint64_t kOpaqueRecent = 600;               // frames: "the main view draws this range opaque"
constexpr std::uint64_t kMaterialRefresh = 64;             // frames: a BLAS's material is resolved again (animated uv maps)
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
    // built right after the z pre-pass (the consumers that shade this frame's surfaces need it before the solid pass: the
    // screen probes); AfterOpaque builds it in frames without a z pre-pass
    passId_ = dev_.addPass(d3d9::inject::Point::AfterDepthPrepass, "rt.scene", [this](d3d9::PassContext& ctx) { record(ctx); }, this, -100);
    passId2_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "rt.scene", [this](d3d9::PassContext& ctx) { record(ctx); }, this, -100);
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
    if (passId2_ != 0) dev_.removePass(passId2_);
    passId_ = passId2_ = 0;
    logStats(true);
    registry_.clear();
    dropDynamic(true, 0);
    destroySkin();
    std::vector<std::uint64_t> ids;
    ids.reserve(blas_.size());
    for (const auto& [id, b] : blas_) ids.push_back(id);
    for (std::uint64_t id : ids) dropBlas(id, false);
    pending_.clear();
    layers_.clear();
    materials_.clear();
    destroyResources();
    dev_.setRtScene({});
    dev_.setStatus("rt.scene", {});
    spdlog::info("rt: scene: off (GPU memory released)");
}

// ---- draw observation -----------------------------------------------------------------------------------------------------

bool RtScene::observes(Kind kind, d3d9::scene::Phase phase) const {
    // characters: the main view's solid pass (also their motion) and the sun cascades' views (this frame's pose, before the main view)
    // (with the skinning pass: the z pre-pass's draws too, they carry this frame's bone matrices before the TLAS is built)
    if (kind == Kind::Skinned) {
        return dynamic_ && ((phase.view == View::Main && (phase.pass == Pass::Solid || (skinWanted_ && phase.pass == Pass::DepthPrepass))) ||
                            phase.view == View::Shadow);
    }
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
        const RtMaterial* m = e.material != 0 ? materials_.get(e.material) : nullptr;
        if (m == nullptr) m = bestMaterial(b);
        const std::string mat = m == nullptr ? std::string("none")
                                             : std::format("#{} '{}' (of {} on the BLAS, albedo {}, uv 0x{:x}{}{})", e.material, m->name,
                                                           b.materials.size(), m->albedo != nullptr ? "yes" : "no", m->uvSet,
                                                           m->masked ? ", masked" : "", m->dead ? ", dead" : "");
        std::string slots;
        for (const Blas::MaterialSlot& ms : b.materials) {
            const RtMaterial* sm = materials_.get(ms.id);
            slots += std::format(" [#{} '{}' rank {}{}]", ms.id, sm != nullptr ? sm->name : std::string("gone"), ms.rank, ms.overlay ? " overlay" : "");
        }
        return s + std::format("; BLAS #{}: vb {}#{} + {} stride {}, ib {}#{} first {} x {} triangles, base {}, bounds ({:.0f} {:.0f} {:.0f})..({:.0f} "
                               "{:.0f} {:.0f}); material {} entity {} op 0x{:x}; rt material {}",
                               b.id, k.vb, k.vbGen, k.vertexOffset, k.stride, k.ib, k.ibGen, k.first, k.primitives, k.baseVertex,
                               b.bounds.min[0], b.bounds.min[1], b.bounds.min[2], b.bounds.max[0], b.bounds.max[1], b.bounds.max[2],
                               b.material, b.entity, b.opDesc, mat) + "; slots" + slots;
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

rt::BlasKey RtScene::keyOf(const d3d9::ReplaceQuery& q) {
    rt::BlasKey key;
    const d3d9::ResourceRef vr = q.vertexBuffer->ref(), ir = q.indexBuffer->ref();
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
    key.strip = q.primitiveType == 5 ? 1u : 0u;
    return key;
}

// The engine's sub-meshes inside a depth-only draw (the main view's z pre-pass and the sun cascades run
// eCMeshResource2::RenderZPass: one draw over a merged index range of the opaque sub-meshes, masked ones alone, no
// eCSubMesh::Render and so no material in the draw tag; docs/research/rt-reflections.md 3.3). eCRenderContextMesh +0x28 =
// its eCMeshResource2; sub-meshes at +0x58 (pointer) / +0x60 (count), 0x48 bytes each: first index +0x28, index count +0x2c,
// eCMaterialResource2* +0x38 [C, Engine.dll RenderZPass 0x7603b0]. Sub-meshes lying wholly inside the draw's range, in
// primitives relative to its start.
bool RtScene::engineSubMeshes(const d3d9::ReplaceQuery& q, std::vector<EngineSub>& out) {
    out.clear();
    if (q.tag == nullptr || q.tag->source != d3d9::draws::Source::Mesh || q.tag->material != nullptr || q.tag->context == nullptr ||
        q.instanced || q.primitiveType != 4) {
        return false;
    }
    const void* mesh = nullptr;
    const std::uint8_t* subs = nullptr;
    std::int32_t count = 0;
    if (!engine::readAt(q.tag->context, 0x28, mesh) || mesh == nullptr || !engine::readAt(mesh, 0x58, subs) || subs == nullptr ||
        !engine::readAt(mesh, 0x60, count) || count <= 0 || count > 4096) {
        return false;
    }
    const std::uint64_t first = q.first, end = first + 3ull * q.primitives;
    for (std::int32_t i = 0; i < count; ++i) {
        std::uint32_t range[2]{};
        const void* material = nullptr;
        const std::uint8_t* sub = subs + static_cast<std::size_t>(i) * 0x48;
        if (!engine::safeRead(sub + 0x28, range, sizeof(range)) || !engine::readAt(sub, 0x38, material)) return false;
        if (material == nullptr || range[1] < 3 || range[0] < first || static_cast<std::uint64_t>(range[0]) + range[1] > end) continue;
        if ((range[0] - first) % 3 != 0) continue;
        out.push_back({static_cast<std::uint32_t>((range[0] - first) / 3), range[1] / 3, material});
    }
    std::sort(out.begin(), out.end(), [](const EngineSub& a, const EngineSub& b) { return a.firstPrimitive < b.firstPrimitive; });
    return !out.empty();
}

// a depth-only draw's materials from the engine's sub-meshes: one sub-mesh over the whole range = the draw's material
// (`tagOut` gets it); several (a merged range) = per-triangle materials of the BLAS (engine segments, resolved once)
void RtScene::learnEngineMaterials(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, d3d9::draws::Tag& tagOut, bool& single) {
    single = false;
    if (q.tag == nullptr || q.tag->material != nullptr) return;
    const bool depthOnly = q.phase.view == View::Shadow || q.phase.pass == Pass::DepthPrepass;
    if (!depthOnly) return;
    const std::uint64_t frame = dev.frameValue();
    if (b.engineChecked != 0 && frame < b.engineChecked + kMaterialRefresh * 4 && !b.engineDead) {
        if (b.engineSingle != nullptr) {
            tagOut = *q.tag;
            tagOut.material = b.engineSingle;
            single = true;
        }
        return;
    }
    std::vector<EngineSub> subs;
    if (!engineSubMeshes(q, subs)) {
        b.engineChecked = frame;
        return;
    }
    if (subs.size() == 1 && subs[0].firstPrimitive == 0 && subs[0].primitives == q.primitives) {
        b.engineSingle = subs[0].material;
        b.engineChecked = frame;
        b.engineDead = false;
        tagOut = *q.tag;
        tagOut.material = b.engineSingle;
        single = true;
        return;
    }
    b.engineChecked = frame;
    if (b.key.primitives < 64) return;
    // a merged range: each sub-mesh's material (the pool image of its diffuse: the depth pass samples none)
    std::vector<Blas::Segment> segs;
    d3d9::draws::Tag t = *q.tag;
    d3d9::ReplaceQuery qs = q;
    qs.tag = &t;
    for (const EngineSub& e : subs) {
        t.material = e.material;
        const std::uint32_t id = materials_.resolve(dev, qs);
        if (id == 0) continue;
        segs.push_back({e.firstPrimitive, e.primitives, 0, id});
    }
    for (const Blas::Segment& old : b.engineSegments) materials_.release(old.material);
    b.engineSegments = std::move(segs);
    b.engineSingle = nullptr;
    b.engineChecked = frame;
    b.engineDead = false;
    b.segmentsFrame = 0;  // the record's segment table again
    ++iv_.engineMerged;
}

std::uint8_t RtScene::viewBit(View v) const { return v == View::Main ? 1 : v == View::Shadow ? 2 : 4; }

void RtScene::observe(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    if (!active_) return;
    if (q.kind == Kind::Skinned) {
        observeSkinned(dev, q);
        return;
    }
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
    const bool speedTree = q.kind == Kind::SpeedTree;
    // SpeedTree branches / fronds: the first rejection of each kind is logged (diagnostics)
    const auto reject = [&](const char* why) {
        if (speedTree && speedTreeLogged_.size() < 16 && speedTreeLogged_.insert(why).second) {
            spdlog::info("rt: scene: SpeedTree draw (op {}, {} {}, stride {}) not taken: {}", q.tag != nullptr ? q.tag->opDesc : 0u,
                         d3d9::scene::name(q.phase.view), d3d9::scene::name(q.phase.pass), q.vertexStride, why);
        }
        skip();
    };
    d3d9::Buffer* vb = q.vertexBuffer;
    d3d9::Buffer* ib = q.indexBuffer;
    // triangle lists; strips (SpeedTree branches / fronds) are converted to lists at build time
    if (q.primitiveType != 4 && q.primitiveType != 5) return reject("not a triangle list / strip");
    if (vb == nullptr || ib == nullptr || q.primitives == 0 || q.engineVs == nullptr) return skip();
    // water reflects, it is not reflected: its meshes are learnt from the main view's refracted / water draws and left out
    // of every view (the sun cascades draw the sea too, untagged: StaticMesh there)
    // the same holds for meshes the main view draws alpha-blended (shore overlays, glass, ...)
    const bool mainBlended = q.phase.view == View::Main && q.blending;
    const bool water = q.kind == Kind::Refracted || (q.tag != nullptr && q.tag->water != d3d9::draws::Water::None);
    if (water || mainBlended) {
        learnExcluded(q, water);
        // the terrain: a z pre-pass of the range (opaque, its BLAS) and blended layers in the solid pass (each its own
        // material on the same geometry): the frame's first layer is the base, its material is the range's
        if (!water && q.vertexBuffer != nullptr && q.indexBuffer != nullptr && !q.vertexBuffer->ringCopied() &&
            !q.indexBuffer->ringCopied()) {
            const rt::BlasKey key = keyOf(q);
            if (const auto it = blasByKey_.find(key); it != blasByKey_.end()) {
                Blas* b = it->second;
                if (b != nullptr && blendLogged_.size() < 200 && blendLogged_.insert(reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->material : nullptr) ^ q.enginePs->hash).second) {
                    spdlog::info("rt: scene: main-view blended draw of BLAS #{} ({} {}): material '{}', blend {}/{}{}", b->id, d3d9::scene::name(q.phase.pass),
                                 q.instanced ? "instanced" : "plain", q.tag != nullptr && q.tag->material != nullptr ? engine::materials::resourceName(q.tag->material) : "?",
                                 q.srcBlend, q.dstBlend, q.blended ? " (alpha-over: a layer)" : "");
                }
                // the draw's instances (a plain draw's rows c33..c35; a batch's c[32 + 4 i + 1 ..], the instance stream is
                // 0, 1, 2, ...): the solid pass's layers (the terrain's base, objects fading in)
                if (b != nullptr && b->state != BlasState::Failed && q.phase.pass == Pass::Solid) {
                    std::uint8_t rank = 0;
                    bool overlay = false;
                    const std::uint32_t id = materialOf(dev, q, *b, rank, overlay);
                    // an untextured blended layer (a wet-sand specular layer with a cube as the diffuse, seen unblended in the
                    // cascades / z pre-pass): the range's untextured materials are overlays on another surface (a textured
                    // object fading in is drawn blended too: not this)
                    if (const RtMaterial* layer = id != 0 ? materials_.get(id) : nullptr; layer != nullptr && (layer->uvSet & abi::kRtUvNoTexture) != 0) {
                        for (Blas::MaterialSlot& m : b->materials) {
                            const RtMaterial* mat = materials_.get(m.id);
                            if (mat != nullptr && (mat->uvSet & abi::kRtUvNoTexture) != 0) m.overlay = true;
                        }
                    }
                    const std::uint32_t count = q.instanced ? std::min(q.instances, kMaxBatch) : 1u;
                    for (std::uint32_t i = 0; id != 0 && i < count; ++i) {
                        const std::uint32_t reg = kInstanceDataBase + (q.instanced ? 4 * i : 0) + 1;
                        if (reg + 2 >= kInstanceDataEnd) break;
                        rt::Rows rows;
                        for (std::uint32_t r = 0; r < 3; ++r) std::memcpy(&rows[r * 4], dev.vsConstant(reg + r), sizeof(float) * 4);
                        if (!rt::finite(rows)) continue;
                        if (rt::Registry::Instance* in = registry_.find(rt::instanceKey(b->id, rows))) applyMaterial(*in, id, rank, overlay, dev.frameValue());
                    }
                }
            } else if (q.phase.pass == Pass::Solid && q.blended && !q.instanced) {
                // a range only drawn as blended layers (no BLAS of its own): the material of its first layer of the frame,
                // for the merged ranges that cover it (the z pre-pass / the cascades draw level meshes in one range)
                learnLayer(dev, q, key);
            }
        }
        return reject(water ? "water" : "blended in the main view");
    }
    if (q.phase.view == View::Main && (q.phase.pass == Pass::Alpha || q.phase.pass == Pass::AlphaLate)) return skip();
    if (q.tag != nullptr && q.tag->material != nullptr && waterMaterials_.contains(q.tag->material)) return skip();
    if (q.blending) return reject("blended");                                        // alpha-blended: not P1
    // SpeedTree leaves (op > 1): cards positioned by their vertex shader (wind, facing), so there is no rest pose to put in the
    // TLAS: only the animated capture of the main view's solid pass stands for them (observeTree), the static BLAS only gives
    // the topology and the material (`animatedOnly`)
    const bool leaf = speedTree && q.tag != nullptr && q.tag->opDesc > 1;
    const bool leafAnimated = leaf && animated_ && q.phase.view == View::Main && q.phase.pass == Pass::Solid;
    if (speedTree && (q.tag == nullptr || (leaf && !leafAnimated))) return reject(q.tag == nullptr ? "untagged" : "leaves (op > 1)");  // P4
    // the vertex shader's verdict (cached by its hash and the draw's instancing: most rejected draws stop here)
    {
        const std::uint64_t vsKey = q.engineVs->hash ^ (q.instanced ? 0x9E3779B97F4A7C15ull : 0ull) ^ (leafAnimated ? 0xD1B54A32D192ED03ull : 0ull);
        auto [it, fresh] = vsVerdict_.try_emplace(vsKey, nullptr);
        if (fresh) {
            const d3d9::ShaderSemantics& sem = q.engineVs->semantics;
            using S = d3d9::ShaderSemantics;
            const auto& instanceData = sem[d3d9::Semantic::InstanceData];
            if ((sem.flags & S::Material) == 0 || (sem.flags & (S::Skinned | S::Vegetation | (leafAnimated ? 0u : S::Leaves))) != 0) it->second = "vertex shader class";
            else if (!instanceData.present() || static_cast<std::uint32_t>(instanceData.reg) != kInstanceDataBase) it->second = "no g_vInstanceData at c32";
            else if (sem[d3d9::Semantic::InvView].present()) it->second = "g_vInvView";  // screen-aligned billboard meshes: camera-dependent
            else if (q.instanced != ((sem.flags & S::Instanced) != 0)) it->second = "instancing of the draw != of the VS";
            if (vsVerdict_.size() > 4096) vsVerdict_.clear();
        }
        if (it->second != nullptr) return reject(it->second);
    }
    if (vb->ringCopied() || ib->ringCopied()) return reject("dynamic buffers");   // dynamic geometry: one frame only
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
    // (the stride / index size hints as the draw's own binding gives them: the first use places the copy in the mesh arena)
    vb->gpu(q.vertexStride);
    ib->gpu(ib->index32() ? 4 : 2);
    const VkDeviceAddress vbAddress = vb->deviceAddress(), ibAddress = ib->deviceAddress();
    if (vbAddress == 0 || ibAddress == 0) return skip();
    const d3d9::ResourceRef vr = vb->ref(), ir = ib->ref();
    const rt::BlasKey key = keyOf(q);

    // the BLAS of the range, one lookup (a range with a BLAS cannot be a water key: learnExcluded drops its BLAS when it turns water)
    const auto known = blasByKey_.find(key);
    if (known == blasByKey_.end() && waterKeys_.contains(key)) return skip();
    // a range the main view draws only blended (the cascades draw it unblended) is not part of the scene
    const bool mainOpaque = q.phase.view == View::Main;
    if (!mainOpaque && !blendedKeys_.empty()) {
        if (const auto it = blendedKeys_.find(key); it != blendedKeys_.end() && it->second + kOpaqueRecent >= dev.frameValue()) {
            const Blas* knownBlas = known != blasByKey_.end() ? known->second : nullptr;
            if (knownBlas == nullptr || knownBlas->opaqueMainFrame + kOpaqueRecent < dev.frameValue()) return skip();
        }
    }
    const std::uint64_t frame = dev.frameValue();
    const double now = dev.secondsSinceCreate();
    Blas* b = nullptr;
    if (known != blasByKey_.end()) {
        b = known->second;
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
        blasByKey_[key] = &nb;
        blasByBuffer_.emplace(vr.id, id);
        if (ir.id != vr.id) blasByBuffer_.emplace(ir.id, id);
        pending_.push_back(id);
        b = &nb;
    }
    if (b->state == BlasState::Failed) return skip();
    if (leaf) b->animatedOnly = true;
    std::uint8_t materialRank = 0;
    bool overlay = false;
    // depth-only draws (z pre-pass, cascades) carry no material: the engine's sub-mesh of the range gives it
    d3d9::draws::Tag engineTag;
    bool single = false;
    learnEngineMaterials(dev, q, *b, engineTag, single);
    d3d9::ReplaceQuery qm = q;
    if (single) qm.tag = &engineTag;
    const std::uint32_t materialId = materialOf(dev, qm, *b, materialRank, overlay);
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
        ++iv_.sightings;
        auto sr = registry_.see(rt::instanceKey(b->id, rows), b->id, rows, owner, flags, alphaWord, view, frame, now);
        rt::Registry::Instance* in = sr.first;
        if (sr.second) noteNewInstance(*b, frame);
        if (in != nullptr && materialId != 0) applyMaterial(*in, materialId, materialRank, overlay, frame);
        if (in != nullptr && speedTree && animated_ && q.phase.view == View::Main && q.phase.pass == Pass::Solid) {
            TreeBatch batch;
            observeTree(dev, q, *b, *in, rows, batch, 0, 1);
        }
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
    TreeBatch treeBatch;
    const bool treeBatchOk = speedTree && animated_ && fromStream && q.phase.view == View::Main && q.phase.pass == Pass::Solid;
    for (std::uint32_t i = 0; i < n; ++i) {
        const float v = fromStream ? index[i] : static_cast<float>(i);
        if (fromStream && v != static_cast<float>(i)) ++indexDeviations_;
        if (!std::isfinite(v) || v < 0.0f || v > 64.0f) continue;
        const std::uint32_t reg = kInstanceDataBase + static_cast<std::uint32_t>(std::floor(4.0f * v + 0.5f)) + 1;
        if (reg + 2 >= kInstanceDataEnd) continue;
        rt::Rows rows;
        if (!rowsAt(reg, rows) || outside(rows)) continue;
        ++iv_.sightings;
        auto sr = registry_.see(rt::instanceKey(b->id, rows), b->id, rows, 0, flags, alphaWord, view, frame, now);
        rt::Registry::Instance* in = sr.first;
        if (sr.second) noteNewInstance(*b, frame);
        if (in != nullptr && materialId != 0) applyMaterial(*in, materialId, materialRank, overlay, frame);
        // the capture's instance k is the batch's k-th instance (the instance stream is 0, 1, 2, ...)
        if (in != nullptr && treeBatchOk && v == static_cast<float>(i)) observeTree(dev, q, *b, *in, rows, treeBatch, i, n);
    }
    if (!instanceIndexChecked_ && fromStream && iv_.observed > 2000) {
        instanceIndexChecked_ = true;
        spdlog::info("rt: scene: instance stream check: {}", indexDeviations_ == 0 ? "TEXCOORD4 = 0, 1, 2, ... in every batch so far"
                                                                                   : std::format("{} instances deviate from 0, 1, 2, ...", indexDeviations_));
    }
}

// a range that gets new instances all the time (kChurnLimit within kChurnWindow frames) is volatile for a while
void RtScene::noteNewInstance(Blas& b, std::uint64_t frame) {
    if (frame >= b.churnStart + kChurnWindow) {
        b.churnStart = frame;
        b.churnNew = 0;
    }
    if (++b.churnNew >= kChurnLimit && b.volatileUntil < frame) {
        if (b.volatileUntil == 0) ++iv_.volatileBlas;
        b.volatileUntil = frame + 4 * kChurnWindow;
    }
    if (b.churnNew >= kChurnLimit) b.volatileUntil = frame + 4 * kChurnWindow;
}

void RtScene::learnExcluded(const d3d9::ReplaceQuery& q, bool water) {
    // water by material (all of its meshes) and draw range; alpha-blended meshes by draw range only, and only until the main
    // view draws that range opaque (objects fading in are drawn blended for a while)
    if (water && q.tag != nullptr && q.tag->material != nullptr && waterMaterials_.insert(q.tag->material).second) {
        spdlog::info("rt: scene: water material {} '{}' left out of the RT scene", q.tag->material,
                     q.tag->materialName != nullptr ? q.tag->materialName : "?");
    }
    if (q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.vertexBuffer->ringCopied() || q.indexBuffer->ringCopied()) return;
    const rt::BlasKey key = keyOf(q);
    if (water) {
        if (!waterKeys_.insert(key).second) return;
        if (const auto it = blasByKey_.find(key); it != blasByKey_.end()) dropBlas(it->second->id, false);
        return;
    }
    // blended: excluded unless the main view also draws the range opaque (multi-pass materials: an opaque base pass plus
    // blended layers on the same geometry, the terrain's; objects fading in)
    const std::uint64_t frame = dev_.frameValue();
    blendedKeys_[key] = frame;
    if (const auto it = blasByKey_.find(key); it != blasByKey_.end()) {
        const Blas* b = it->second;
        if (b != nullptr && b->opaqueMainFrame + kOpaqueRecent < frame) dropBlas(b->id, false);
    }
}

std::uint32_t RtScene::materialOf(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, std::uint8_t& rank, bool& overlay) {
    // layers blended otherwise than alpha-over (additive caustics, multiplied detail) are overlays, never a base
    if (q.blending && !q.blended) return 0;
    rank = q.phase.view != View::Main ? 1 : q.phase.pass != Pass::Solid ? 2 : q.blending ? 3 : 4;
    const std::uint64_t frame = dev.frameValue();
    const std::uint64_t sig = RtMaterials::signature(q);
    if (b.memoFrame == frame && b.memoSig == sig && b.memoRank == rank && b.memoBlending == q.blending && b.memoId != 0) {
        overlay = b.memoOverlay;
        return b.memoId;
    }
    for (Blas::MaterialSlot& m : b.materials) {
        if (m.sig != sig) continue;
        const RtMaterial* mat = materials_.get(m.id);
        if (mat != nullptr && !mat->dead && frame < m.resolved + kMaterialRefresh) {
            m.used = frame;
            m.rank = std::max(m.rank, rank);
            // a range whose untextured material the main view draws as a blended layer: an overlay (the cascades and the
            // z pre-pass may have shown it first, unblended)
            if (q.blending && (mat->uvSet & shader::abi::kRtUvNoTexture) != 0) m.overlay = true;
            overlay = m.overlay;
            b.memoFrame = frame;
            b.memoSig = sig;
            b.memoRank = rank;
            b.memoBlending = q.blending;
            b.memoId = m.id;
            b.memoOverlay = m.overlay;
            return m.id;
        }
        // a texture died or the refresh is due (animated uv maps): resolved again
        const std::uint32_t id = materials_.resolve(dev, q);
        if (id == 0) return 0;
        materials_.release(m.id);
        b.memoId = 0;  // the memoized id was just released
        m.id = id;
        m.resolved = m.used = frame;
        m.rank = std::max(m.rank, rank);
        overlay = m.overlay;
        return id;
    }
    const std::uint32_t id = materials_.resolve(dev, q);
    if (id == 0) return 0;
    if (b.materials.size() >= kMaterialSlots) {
        const auto lru = std::min_element(b.materials.begin(), b.materials.end(), [](const auto& x, const auto& y) { return x.used < y.used; });
        if (b.memoId == lru->id) b.memoId = 0;
        materials_.release(lru->id);
        b.materials.erase(lru);
    }
    // alpha-blended with a texture as the opacity: caustics / decals over a floor (the cascades draw them unblended); a
    // blended layer that is no terrain layer (those: a diffuse texture, vertex-colour opacity): a wet specular layer with a
    // reflection map, an object fading in, an untextured strip
    engine::materials::ShaderInfo info;
    const bool known = engine::materials::active(info, q.tag != nullptr ? q.tag->material : nullptr);
    overlay = known && info.blendMode == 2 && info.opacity.source == engine::materials::Source::Sampler;
    if (q.blending && !(known && info.diffuse.source == engine::materials::Source::Sampler && info.opacity.source == engine::materials::Source::VertexColor)) {
        overlay = true;
    }
    b.materials.push_back({sig, id, frame, frame, rank, overlay});
    return id;
}

void RtScene::learnLayer(d3d9::Device& dev, const d3d9::ReplaceQuery& q, const rt::BlasKey& key) {
    const std::uint64_t frame = dev.frameValue();
    Layer& l = layers_[key];
    if (l.seen == frame) return;  // the frame's first layer is the base
    l.seen = frame;
    const std::uint64_t sig = RtMaterials::signature(q);
    const RtMaterial* current = l.material != 0 ? materials_.get(l.material) : nullptr;
    if (current != nullptr && !current->dead && sig == l.sig && frame < l.resolved + kMaterialRefresh) return;
    // only terrain-like layers (a diffuse texture, vertex-colour opacity) are a base; overlays (caustics, specular layers,
    // decals) are not
    engine::materials::ShaderInfo info;
    if (!engine::materials::active(info, q.tag != nullptr ? q.tag->material : nullptr) || info.diffuse.source != engine::materials::Source::Sampler ||
        info.opacity.source != engine::materials::Source::VertexColor) {
        return;
    }
    const std::uint32_t id = materials_.resolve(dev, q);
    if (id == 0) return;
    const RtMaterial* m = materials_.get(id);
    if (m == nullptr || (m->uvSet & abi::kRtUvNoTexture) != 0) {
        materials_.release(id);
        return;
    }
    if (l.material != 0) materials_.release(l.material);
    l.material = id;
    l.sig = sig;
    l.resolved = frame;
}

void RtScene::applyMaterial(rt::Registry::Instance& in, std::uint32_t id, std::uint8_t rank, bool overlay, std::uint64_t frame) {
    if (rank < in.materialRank) return;                          // a better view gave it
    if (overlay && in.material != 0) return;                     // an overlay only while nothing else is known
    // blended layers: the first draw of the frame at this rank counts (the base); opaque passes over the same surface: the
    // last one (it covers the earlier ones in the depth buffer)
    if (rank != 4 && in.sightFrame == frame && in.sightRank == rank) return;
    in.sightFrame = frame;
    in.sightRank = rank;
    in.material = id;
    in.materialRank = rank;
}

RtScene::MaterialFields RtScene::materialFields(const RtMaterial* m) {
    MaterialFields f;
    const auto slotOf = [](d3d9::Texture* t) -> std::uint32_t {
        return t != nullptr && !t->isNull() && t->image().layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? t->bindlessSlot() : 0u;
    };
    if (m == nullptr) {
        f.uvSet = abi::kRtUvNoTexture | abi::kRtUvHeuristic | (3u << abi::kRtUvOpacityShift);
        f.uvMap[0][0] = 1.0f;
        f.uvMap[1][1] = 1.0f;
        const std::uint32_t grey = 0xFF808080u;
        std::memcpy(&f.uvMap[0][3], &grey, 4);
        return f;
    }
    f.albedoTex = slotOf(m->albedo);
    f.opacityTex = (m->uvSet & abi::kRtUvOpacityTexture) != 0 ? slotOf(m->opacity) : f.albedoTex;
    f.albedoSampler = m->sampler;
    f.uvSet = m->uvSet | (f.albedoTex == 0 ? abi::kRtUvNoTexture : 0u);
    for (int row = 0; row < 2; ++row) {
        for (int c = 0; c < 3; ++c) f.uvMap[row][c] = m->uvMap[row][c];
    }
    std::memcpy(&f.uvMap[0][3], &m->colour, 4);
    std::memcpy(&f.uvMap[1][3], &f.opacityTex, 4);
    return f;
}

void RtScene::refreshSegments(Blas& b) {
    b.segments.clear();
    const rt::BlasKey& k = b.key;
    const std::uint64_t end = static_cast<std::uint64_t>(k.first) + 3ull * k.primitives;
    const auto range = blasByBuffer_.equal_range(k.vb);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == b.id) continue;
        const Blas* sub = findBlas(it->second);
        if (sub == nullptr) continue;
        const rt::BlasKey& s = sub->key;
        if (s.strip != 0 || s.vb != k.vb || s.ib != k.ib || s.vbGen != k.vbGen || s.ibGen != k.ibGen || s.vertexOffset != k.vertexOffset || s.stride != k.stride ||
            s.baseVertex != k.baseVertex || s.index32 != k.index32 || s.first < k.first || (s.first - k.first) % 3 != 0 ||
            static_cast<std::uint64_t>(s.first) + 3ull * s.primitives > end) {
            continue;
        }
        if (std::none_of(sub->materials.begin(), sub->materials.end(), [](const auto& m) { return m.rank >= 3 && !m.overlay; })) continue;
        b.segments.push_back({(s.first - k.first) / 3, s.primitives, sub->id});
    }
    for (const auto& [s, l] : layers_) {
        if (l.material == 0 || s.strip != 0 || s.vb != k.vb || s.ib != k.ib || s.vbGen != k.vbGen || s.ibGen != k.ibGen || s.vertexOffset != k.vertexOffset ||
            s.stride != k.stride || s.baseVertex != k.baseVertex || s.index32 != k.index32 || s.first < k.first || (s.first - k.first) % 3 != 0 ||
            static_cast<std::uint64_t>(s.first) + 3ull * s.primitives > end) {
            continue;
        }
        b.segments.push_back({(s.first - k.first) / 3, s.primitives, 0, l.material});
    }
    // the engine's sub-meshes of the merged range (depth-only draws): where the main view showed no sub-range
    const std::size_t mainSegments = b.segments.size();
    for (const Blas::Segment& e : b.engineSegments) {
        const RtMaterial* m = materials_.get(e.material);
        if (m == nullptr || m->dead) {
            b.engineDead = true;
            continue;
        }
        b.segments.push_back(e);
    }
    iv_.engineSegments += b.segments.size() - mainSegments;
    // (the main view's first where both cover a range: stable sort, the main view's were pushed first)
    std::stable_sort(b.segments.begin(), b.segments.end(), [](const auto& x, const auto& y) { return x.firstPrimitive < y.firstPrimitive; });
    // overlapping sub-ranges: the first one
    std::uint32_t covered = 0;
    std::erase_if(b.segments, [&](const Blas::Segment& seg) {
        if (seg.firstPrimitive < covered) return true;
        covered = seg.firstPrimitive + seg.primitiveCount;
        return false;
    });
}

const RtMaterial* RtScene::bestMaterial(const Blas& b) const {
    const Blas::MaterialSlot* best = nullptr;
    for (const Blas::MaterialSlot& m : b.materials) {
        if (m.overlay && b.materials.size() > 1) continue;
        if (best == nullptr || m.rank > best->rank || (m.rank == best->rank && m.used > best->used)) best = &m;
    }
    return best != nullptr ? materials_.get(best->id) : nullptr;
}

void RtScene::textureReleased(const d3d9::Texture* texture) {
    if (active_) materials_.textureReleased(texture);
}

void RtScene::bufferChanged(std::uint64_t bufferId) {
    if (!active_) return;
    std::erase_if(waterKeys_, [&](const rt::BlasKey& k) { return k.vb == bufferId || k.ib == bufferId; });
    std::erase_if(blendedKeys_, [&](const auto& e) { return e.first.vb == bufferId || e.first.ib == bufferId; });
    std::erase_if(layers_, [&](auto& e) {
        if (e.first.vb != bufferId && e.first.ib != bufferId) return false;
        if (e.second.material != 0) materials_.release(e.second.material);
        return true;
    });
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
    for (const Blas::MaterialSlot& m : b->materials) materials_.release(m.id);
    b->materials.clear();
    b->memoId = 0;
    for (const Blas::Segment& e : b->engineSegments) materials_.release(e.material);
    b->engineSegments.clear();
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
    if (b->listIndices.buffer != VK_NULL_HANDLE) {
        blasBytes_ -= std::min(blasBytes_, static_cast<std::uint64_t>(b->listIndices.size));
        vk::Context* ctx = &dev_.ctx();
        vk::rt::AddressBuffer old = b->listIndices;
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
    const std::uint32_t count = k.strip != 0 ? k.primitives + 2 : k.primitives * 3;
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(count) * isize);
    if (!b.ib->readCpu(k.first * isize, count * isize, idx.data())) {
        return fail(kReasonIndexRange, std::format("index range {} + {} outside the index buffer ({} bytes)", k.first, count, b.ib->size()));
    }
    const auto at = [&](std::uint32_t i) {
        if (isize == 4) {
            std::uint32_t v;
            std::memcpy(&v, idx.data() + static_cast<std::size_t>(i) * 4, 4);
            return v;
        }
        std::uint16_t v;
        std::memcpy(&v, idx.data() + static_cast<std::size_t>(i) * 2, 2);
        return static_cast<std::uint32_t>(v);
    };
    std::uint32_t lo = UINT32_MAX, hi = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t v = at(i);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    b.triangles = k.primitives;
    if (k.strip != 0) {
        // the strip as a list: triangle t = (i[t], i[t+1], i[t+2]), every second one with its winding swapped (culling is off
        // for every instance, the winding only orients the geometric normal); degenerate triangles (strip restarts) dropped
        std::vector<std::uint32_t> list;
        list.reserve(static_cast<std::size_t>(k.primitives) * 3);
        for (std::uint32_t t = 0; t < k.primitives; ++t) {
            const std::uint32_t a = at(t), c1 = at(t + 1), c2 = at(t + 2);
            if (a == c1 || c1 == c2 || a == c2) continue;
            list.push_back(a);
            list.push_back((t & 1u) != 0 ? c2 : c1);
            list.push_back((t & 1u) != 0 ? c1 : c2);
        }
        if (list.empty()) return fail(kReasonIndexRange, "a strip of degenerate triangles only");
        b.triangles = static_cast<std::uint32_t>(list.size() / 3);
        const std::size_t bytes = (list.size() * isize + 3) & ~static_cast<std::size_t>(3);  // whole words (the shaders' loads)
        b.listData.assign(bytes, 0);
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (isize == 4) {
                std::memcpy(b.listData.data() + i * 4, &list[i], 4);
            } else {
                const std::uint16_t v = static_cast<std::uint16_t>(list[i]);
                std::memcpy(b.listData.data() + i * 2, &v, 2);
            }
        }
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
    bool copied = false;  // strip conversions copied into their index buffers: visible to the builds (and the shaders) first
    const auto flush = [&] {
        if (infos.empty()) return;
        if (copied) {
            barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            copied = false;
        }
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
        if (k.strip != 0) {
            // the converted list into a device buffer of its own (through the frame's ring)
            const VkDeviceSize bytes = b->listData.size();
            const vk::RingAlloc up = dev_.frames().allocate(bytes, 16);
            if (up.ptr == nullptr ||
                !b->listIndices.create(ctx, bytes,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       false, 16, "rt scene strip indices")) {
                b->state = BlasState::Failed;
                ++iv_.failed;
                registry_.removeBlas(b->id);
                continue;
            }
            std::memcpy(up.ptr, b->listData.data(), bytes);
            const VkBufferCopy region{up.offset, 0, bytes};
            vkCmdCopyBuffer(cmd, up.buffer, b->listIndices.buffer, 1, &region);
            b->listData.clear();
            b->listData.shrink_to_fit();
            blasBytes_ += bytes;
            copied = true;
        }
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
        tri.indexData.deviceAddress = k.strip != 0 ? b->listIndices.address : b->ibAddress + static_cast<VkDeviceSize>(k.first) * isize;
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &b->triangles, &sizes);
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
        ranges.push_back({b->triangles, 0, 0, 0});
        b->state = BlasState::Built;
        ++b->serial;
        blasBytes_ += b->as.storage.size;
        built.push_back(b->id);
        ++iv_.built;
        iv_.builtTriangles += b->triangles;
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

// ---- characters (M10 P3) -------------------------------------------------------------------------------------------------------

// the capture variant of a vertex shader (d3d9::VariantSlot::Capture): after its clip position, the world position (the
// main view's inverse view-projection; the skinned draws use its camera) and the TEXCOORD0 input go to the push constant
// prevVs's buffer (16-byte header: slots) at the vertex's index-buffer value (gl_VertexIndex - the draw's vertexOffset,
// DrawBlock pad1)
bool RtScene::captureOptions(const d3d9::Shader& s, shader::GlslOptions& o) {
    if (s.stage != shader::Stage::Vertex) return false;
    std::string tc = "vec4(0.0)";
    for (const auto& in : s.reflection.vertexInputs) {
        if (in.usage == shader::Usage::TexCoord && in.usageIndex == 0) tc = std::format("v{}", in.location);
    }
    o.prelude = std::format(
        "#extension GL_EXT_buffer_reference_uvec2 : require\n"
        "layout(buffer_reference, std430, buffer_reference_align = 16) buffer RvkCapture {{\n"
        "    uvec4 info;  // x slots, yz the previous frame's capture of the draw (0 = none), w its slots\n"
        "    vec4 v[];    // per slot: world position (w = 1), TEXCOORD0\n"
        "}};\n"
        "// the previous frame's world position of the vertex (w = 1; 0 = unknown): per-object motion (enh/gbuffer)\n"
        "layout(location = {}) out vec4 rvk_prevWorld;\n"
        "// the operand the shader multiplies with its view-projection: the world position, in any view (GlslOptions::dotCaptures)\n"
        "vec4 rvk_capW = vec4(0.0);\n",
        kPrevWorldLocation);
    if (const auto& vp = s.semantics[d3d9::Semantic::ViewProjection]; vp.present()) {
        o.dotCaptures.push_back({static_cast<std::uint32_t>(vp.reg), "rvk_capW"});
    }
    o.epilogue = std::format(
        "    rvk_prevWorld = vec4(0.0);\n"
        "    {{\n"
        "        uvec2 rvk_capAddr = uvec2(pc.prevVs);\n"
        "        if (rvk_capAddr != uvec2(0u)) {{\n"
        "            RvkCapture rvk_cap = RvkCapture(rvk_capAddr);\n"
        "            int rvk_slot = gl_VertexIndex - int(pc.draw.pad1);\n"
        "            uvec4 rvk_info = rvk_cap.info;\n"
        "            uint rvk_slots = rvk_info.x & 0x7FFFFFFFu;\n"
        "            // the main view: the clip position back to the world; a cascade's view (bit 31 of the header's slots): the operand the\n"
        "            // shader multiplied with its view-projection\n"
        "            vec4 rvk_w = (rvk_info.x & 0x80000000u) != 0u ? rvk_capW : pc.frame.invViewProj * rvk_pos;\n"
        "            if (rvk_slot >= 0 && uint(rvk_slot) < rvk_slots && abs(rvk_w.w) > 1e-20) {{\n"
        "                vec3 rvk_world = rvk_w.xyz / rvk_w.w;\n"
        "                uint rvk_at = uint(gl_InstanceIndex) * rvk_slots + uint(rvk_slot);  // instanced draws: one run of slots per instance\n"
        "                rvk_cap.v[rvk_at * 2] = vec4(rvk_world, 1.0);\n"
        "                rvk_cap.v[rvk_at * 2 + 1] = {};\n"
        "                rvk_prevWorld = vec4(rvk_world, 0.0);\n"
        "                if (rvk_info.yz != uvec2(0u) && uint(rvk_slot) < rvk_info.w) {{\n"
        "                    vec4 rvk_prev = RvkCapture(rvk_info.yz).v[rvk_slot * 2];\n"
        "                    if (rvk_prev.w == 1.0) rvk_prevWorld = vec4(rvk_prev.xyz, 1.0);\n"
        "                }}\n"
        "            }}\n"
        "        }}\n"
        "    }}",
        tc);
    return true;
}

bool RtScene::dynamicSupported() const { return supported() && dev_.ctx().features().vertexStores; }

void RtScene::setDynamic(const void* consumer, bool on) {
    struct Sync {
        RtScene& s;
        ~Sync() {
            const bool a = !s.animConsumers_.empty() && s.dynamic_;
            if (a != s.animated_) {
                s.animated_ = a;
                if (!a) s.animatedKeys_.clear();
            }
        }
    } sync{*this};
    if (on) {
        dynamicConsumers_.insert(consumer);
    } else {
        dynamicConsumers_.erase(consumer);
    }
    const bool want = !dynamicConsumers_.empty() && dynamicSupported();
    if (want == dynamic_) return;
    dynamic_ = want;
    if (want) {
        if (!captureRegistered_) {
            dev_.setVariantProvider(d3d9::VariantSlot::Capture, {"cap5", captureOptions});
            captureRegistered_ = true;
        }
        spdlog::info("rt: scene: characters on (skinned solid draws captured each frame)");
    } else {
        dropDynamic(true, 0);
        spdlog::info("rt: scene: characters off");
    }
}

// the capture requests of a new frame: the list of the frame before is kept (a character the cascades did not draw this frame
// is built from its main-view capture of the previous frame)
void RtScene::rollDyn(std::uint64_t frame) {
    if (dynFrame_ == frame) return;
    if (dynFrame_ + 1 == frame) {
        dynPrev_ = std::move(dyn_);
    } else {
        dynPrev_.clear();
    }
    dyn_.clear();
    skinJobs_.clear();
    dynFrame_ = frame;
    dynTrees_ = 0;
}

void RtScene::setAnimated(const void* consumer, bool on, float rangeUnits) {
    if (on) {
        animConsumers_.insert(consumer);
        animRange_ = rangeUnits;
    } else {
        animConsumers_.erase(consumer);
    }
    const bool want = !animConsumers_.empty() && dynamic_;
    if (want != animated_) spdlog::info("rt: scene: animated SpeedTrees {} (within {:.0f} m)", want ? "on" : "off", animRange_ / 100.0f);
    animated_ = want;
    if (!animated_) animatedKeys_.clear();
    vsVerdict_.clear();
}

// An animated SpeedTree (the draw of the main view's solid pass): its vertex shader's capture variant writes the wind-animated
// world positions into the frame's ring; the next frame's AfterDepthPrepass build refits a BLAS of its own from them
// (buildDynamic), and the TLAS takes it instead of the rest-pose instance of the registry (same key)
void RtScene::observeTree(d3d9::Device& dev, const d3d9::ReplaceQuery& q, Blas& b, const rt::Registry::Instance& in, const rt::Rows& rows, TreeBatch& batch,
                          std::uint32_t index, std::uint32_t count) {
    ++iv_.treeDraws;
    if (batch.failed || b.state != BlasState::Built || b.maxVertex == 0 || b.maxVertex >= (1u << 20) || b.triangles == 0 || q.indexBuffer == nullptr) return;
    const d3d9::scene::CameraBlock& cam = dev.camera().valid ? dev.camera() : dev.lastCamera();
    if (!cam.valid) return;
    const float dx = rows[3] - cam.position[0], dy = rows[7] - cam.position[1], dz = rows[11] - cam.position[2];
    if (dx * dx + dy * dy + dz * dz > animRange_ * animRange_) {
        ++iv_.treeFar;
        return;
    }
    const std::uint64_t frame = dev.frameValue();
    rollDyn(frame);
    if (dynTrees_ >= kMaxAnimatedTrees || dyn_.size() >= 512) return;
    for (const DynDraw& d : dyn_) {
        if (d.key == in.key && d.main) return;
    }
    const bool index32 = b.key.index32 != 0;
    const VkDeviceAddress indices = b.key.strip != 0 ? b.listIndices.address
                                                     : b.ibAddress + static_cast<VkDeviceAddress>(b.key.first) * (index32 ? 4u : 2u);
    if (indices == 0) return;
    const std::uint32_t slots = b.maxVertex + 1;
    if (batch.base == 0) {
        // the draw's capture buffer: a header + `count` runs of `slots` vertices (an instanced batch's instances, whether or
        // not they are all close enough: instance k always writes run k)
        const vk::RingAlloc a = dev.frames().allocate(16 + static_cast<VkDeviceSize>(slots) * 32 * count, 16);
        if (a.ptr == nullptr) {
            batch.failed = true;
            return;
        }
        const std::uint32_t header[4] = {slots, 0, 0, 0};
        std::memcpy(a.ptr, header, 16);
        batch.base = a.address;
        batch.slots = slots;
        dev.requestVertexCapture(a.address, q.instanced);
    }
    if (batch.slots != slots) return;
    DynBlas& db = dynBlas_[in.key];
    if (db.id == 0) {
        db.id = nextDynId_++;
        db.updates = static_cast<std::uint32_t>((in.key >> 8) % 30);  // refits / rebuilds of the trees do not all fall in one frame
    }
    DynDraw d{in.key, batch.base + static_cast<VkDeviceAddress>(index) * slots * 32, indices, slots, b.triangles, index32, false, true, b.id, batch.base};
    dyn_.push_back(d);
    ++dynTrees_;
    ++iv_.treeCaptured;
}

// the vertex layout the skinning pass reads (rt_skin.comp): stream 0 only, POSITION F3, BLENDWEIGHT0 F4, BLENDINDICES0 UBYTE4,
// TEXCOORD0 F2; morph targets (POSITION1..3 in streams 1..3, the heads') only while their weights (the vertex shader's c31.xyz) are
// zero, then the vertex shader adds nothing of them; a draw with active morph targets keeps the vertex shader capture; `offsets`
// packs the four byte offsets
bool RtScene::skinEligible(const d3d9::ReplaceQuery& q, const float* morphWeights, std::uint32_t& offsets) const {
    if (q.decl == nullptr || q.vertexBuffer == nullptr || q.vertexStride == 0 || (q.vertexStride & 3u) != 0 || q.vertexStride > 255 ||
        q.vertexBuffer->ringCopied() || (q.vertexOffset & 3u) != 0) {
        return false;
    }
    int pos = -1, bw = -1, bi = -1, tc = -1;
    bool morph = false;
    for (const d3d9::VertexElement& e : q.decl->elements()) {
        if (e.stream == d3d9::kDeclEndStream) break;
        if (e.stream != 0) {
            morph = true;
            continue;
        }
        if (e.usage == 0 && e.usageIndex == 0 && e.type == 2) pos = e.offset;
        else if (e.usage == 0) morph = true;  // POSITION1..3
        else if (e.usage == 1 && e.usageIndex == 0 && e.type == 3) bw = e.offset;
        else if (e.usage == 2 && e.usageIndex == 0 && e.type == 5) bi = e.offset;
        else if (e.usage == 5 && e.usageIndex == 0 && e.type == 1) tc = e.offset;
    }
    for (int o : {pos, bw, bi, tc}) {
        if (o < 0 || o > 252 || (o & 3) != 0) return false;
    }
    if (morph && (std::fabs(morphWeights[0]) > 1e-6f || std::fabs(morphWeights[1]) > 1e-6f || std::fabs(morphWeights[2]) > 1e-6f)) return false;
    offsets = static_cast<std::uint32_t>(pos) | (static_cast<std::uint32_t>(bw) << 8) | (static_cast<std::uint32_t>(bi) << 16) |
              (static_cast<std::uint32_t>(tc) << 24);
    return true;
}

void RtScene::observeSkinned(d3d9::Device& dev, const d3d9::ReplaceQuery& q) {
    if (!dynamic_ || q.primitiveType != 4 || q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.primitives == 0 || q.instanced ||
        q.indexBuffer->ringCopied()) {
        return;
    }
    const std::uint64_t frame = dev.frameValue();
    rollDyn(frame);
    if (dyn_.size() >= 512) return;
    // a cascade's view (the sun's shadow maps are drawn before the main view of the frame, with this frame's pose) or the main view's
    const bool mainView = q.phase.view == View::Main;
    const bool solid = mainView && q.phase.pass == Pass::Solid;
    // the skinning pass takes the draws of the z pre-pass and of the cascades (before the TLAS is built); the solid pass keeps
    // its vertex shader capture (the previous main-view positions: motion vectors, and the fallback for a character the others missed)
    std::uint32_t skinOffsets = 0;
    const bool skin = skinWanted_ && !solid && skinReady() && skinEligible(q, dev.vsConstant(31), skinOffsets);
    if (skinWanted_ && !solid) ++(skin ? iv_.skinDraws : iv_.skinIneligible);
    if (skinWanted_ && !solid && !skin && skinReady_ && q.decl != nullptr && skinLogged_.insert(q.decl->id()).second && skinLogged_.size() <= 8) {
        std::string els;
        for (const d3d9::VertexElement& e : q.decl->elements()) {
            if (e.stream == d3d9::kDeclEndStream) break;
            els += std::format(" [s{} +{} type {} usage {}.{}]", e.stream, e.offset, e.type, e.usage, e.usageIndex);
        }
        spdlog::info("rt: skin: a draw the skinning pass cannot take: stride {}, vertex offset {}, buffer ring-copied {}:{}", q.vertexStride, q.vertexOffset,
                     q.vertexBuffer != nullptr && q.vertexBuffer->ringCopied(), els);
    }
    if (mainView && !solid && !skin) return;  // the pre-pass's draws only feed the skinning pass
    // verify: only the cascades' draws (the vertex shader capture of the same draw is the reference)
    if (skin && skinVerify_ && mainView) return;
    // the draw: entity x buffers x range (the later passes of a character draw the same range again: the first one counts)
    const d3d9::ResourceRef vr = q.vertexBuffer->ref(), ir = q.indexBuffer->ref();
    std::uint64_t key = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    mix(reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->entity : nullptr));
    mix(vr.id);
    mix(vr.generation);
    mix(ir.id);
    mix(ir.generation);
    mix(q.first);
    mix(q.primitives);
    mix(static_cast<std::uint32_t>(q.baseVertex));
    mix(q.vertexOffset);
    for (const DynDraw& d : dyn_) {
        if (d.key == key && d.skin == skin && (skin || d.main == mainView)) return;
    }
    if (solid && skinWanted_ && skinReady_) {
        // diagnostics: a solid-pass character draw without a skinning job of this frame (the pose of the previous frame is used for it)
        bool has = false;
        for (const DynDraw& d : dyn_) has = has || (d.key == key && d.skin);
        ++(has ? iv_.solidWithSkin : iv_.solidNoSkin);
        if (!has && skinLogged_.size() < 12 && skinLogged_.insert(key).second) {
            std::uint32_t o = 0;
            spdlog::info("rt: skin: a solid-pass draw without a skinning job: entity {}, primitives {}, morph weights {:.3f} {:.3f} {:.3f}, eligible {}, maxIndex {}, jobs {}",
                         reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->entity : nullptr), q.primitives, dev.vsConstant(31)[0], dev.vsConstant(31)[1],
                         dev.vsConstant(31)[2], skinEligible(q, dev.vsConstant(31), o), dynBlas_[key].maxIndex, skinJobs_.size());
        }
    }
    DynBlas& b = dynBlas_[key];
    if (b.id == 0) b.id = nextDynId_++;
    d3d9::Buffer* ib = q.indexBuffer;
    const bool index32 = ib->index32();
    const std::uint32_t isize = index32 ? 4u : 2u;
    if (!b.scanned) {
        // the highest index of the range (the capture's slot count; D3D9's minIndex / numVertices are not trusted)
        b.scanned = true;
        std::vector<std::uint8_t> idx(static_cast<std::size_t>(q.primitives) * 3 * isize);
        if (ib->readCpu(q.first * isize, static_cast<std::uint32_t>(idx.size()), idx.data())) {
            std::uint32_t m = 0;
            for (std::size_t i = 0; i < idx.size(); i += isize) {
                std::uint32_t v = 0;
                std::memcpy(&v, idx.data() + i, isize);
                m = std::max(m, v);
            }
            b.maxIndex = m;
        }
    }
    if (b.maxIndex == 0 || b.maxIndex >= (1u << 20)) return;
    const std::uint32_t slots = b.maxIndex + 1;
    ib->gpu(isize);
    const VkDeviceAddress ibAddress = ib->deviceAddress();
    if (ibAddress == 0) return;
    // the skinning pass reads the draw's static vertex buffer by address
    VkDeviceAddress vertexAddress = 0;
    if (skin) {
        q.vertexBuffer->gpu(q.vertexStride);
        const VkDeviceAddress vb = q.vertexBuffer->deviceAddress();
        const std::int64_t at = static_cast<std::int64_t>(q.vertexOffset) + static_cast<std::int64_t>(q.baseVertex) * q.vertexStride;
        if (vb != 0 && at >= 0) vertexAddress = vb + static_cast<VkDeviceAddress>(at);
    }
    const bool useSkin = skin && vertexAddress != 0;
    if (mainView && !solid && !useSkin) return;
    const vk::RingAlloc a = dev.frames().allocate(16 + static_cast<VkDeviceSize>(slots) * 32, 16);
    if (a.ptr == nullptr) return;
    // header: slots, the previous frame's capture of this draw (its vertex shader reads the previous positions: motion)
    // (bit 31 of the slots: a cascade's view, whose vertex shader's world position operand is captured instead of the clip position)
    std::uint32_t header[4] = {slots | ((mainView || useSkin) ? 0u : 0x80000000u), 0, 0, 0};
    if (mainView && !useSkin && b.lastCapture != 0 && b.lastCaptureFrame + 1 == frame) {
        header[1] = static_cast<std::uint32_t>(b.lastCapture);
        header[2] = static_cast<std::uint32_t>(b.lastCapture >> 32);
        header[3] = b.lastSlots;
    }
    std::memcpy(a.ptr, header, 16);
    if (useSkin) {
        SkinJob j;
        j.vertices = vertexAddress;
        j.out = a.address;
        j.slots = slots;
        j.stride = q.vertexStride;
        j.offsets = skinOffsets;
        // the bone matrices: c32 .. c231 of this draw's vertex shader constants
        const vk::RingAlloc bones = dev.frames().allocate(200u * 16u, 16);
        if (bones.ptr == nullptr) return;
        std::memcpy(bones.ptr, dev.vsConstant(kInstanceDataBase), 200u * 16u);
        j.bones = bones.address;
        if (skinVerify_) {
            // the vertex shader's own result for the same draw (the cascade view's capture variant), zeroed: slots it did not write have w = 0
            const vk::RingAlloc ref = dev.frames().allocate(16 + static_cast<VkDeviceSize>(slots) * 32, 16);
            if (ref.ptr != nullptr) {
                std::uint32_t rh[4] = {slots | 0x80000000u, 0, 0, 0};
                std::memcpy(ref.ptr, rh, 16);
                std::memset(static_cast<std::uint8_t*>(ref.ptr) + 16, 0, static_cast<std::size_t>(slots) * 32);
                dev.requestVertexCapture(ref.address);
                j.ref = ref.address;
            }
        }
        skinJobs_.push_back(j);
    } else {
        dev.requestVertexCapture(a.address);
    }
    // the draw's material (diffuse + uv map), resolved again when it changes (the solid pass's draws carry it)
    const std::uint64_t sig = RtMaterials::signature(q);
    if (solid && (b.material == 0 || b.sig != sig || frame >= b.resolved + 600)) {
        if (const std::uint32_t id = materials_.resolve(dev, q); id != 0) {
            if (b.material != 0) materials_.release(b.material);
            b.material = id;
            b.sig = sig;
            b.resolved = frame;
        }
    }
    DynDraw d{key, a.address, ibAddress + static_cast<VkDeviceAddress>(q.first) * isize, slots, q.primitives, index32, false};
    d.main = mainView && !useSkin;
    d.skin = useSkin;
    dyn_.push_back(d);
}

// the frame's captured characters -> one BLAS each: refit while the topology holds (30 times), else a full build
void RtScene::buildDynamic(VkCommandBuffer cmd, std::uint64_t frame) {
    animatedKeys_.clear();
    if (frame % 120 == 0) dropDynamic(false, frame);
    // Per key the newest capture that ran: this frame's (the cascades' views before the main view, all of them at AfterOpaque),
    // else the previous frame's (its main-view solid pass; its ring memory lives one frame more)
    dynUse_.clear();
    if (!dynamic_) return;
    if (dynFrame_ != frame) rollDyn(frame);
    recordSkin(cmd);
    {
        std::unordered_set<std::uint64_t> taken;
        for (const DynDraw& d : dyn_) {
            if (taken.contains(d.key)) continue;
            const VkDeviceAddress asked = d.requested != 0 ? d.requested : d.capture;
            if (!dev_.vertexCaptured(asked)) continue;
            taken.insert(d.key);
            dynUse_.push_back(d);
            dynUse_.back().built = false;
            ++iv_.capCurrent;
            if (!d.tree) {
                ++iv_.charCurrent;
                if (d.skin) ++iv_.charSkinned;
            }
        }
        for (const DynDraw& d : dynPrev_) {
            if (taken.contains(d.key)) continue;
            const VkDeviceAddress asked = d.requested != 0 ? d.requested : d.capture;
            if (!dev_.vertexCapturedPrev(asked)) continue;
            taken.insert(d.key);
            dynUse_.push_back(d);
            dynUse_.back().built = false;
            ++iv_.capPrevious;
            if (!d.tree) ++iv_.charPrevious;
        }
        // the motion chain (the main view's vertex shader reads the previous main-view capture of the draw)
        for (const DynDraw& d : dyn_) {
            if (!d.main || d.tree) continue;
            const VkDeviceAddress asked = d.requested != 0 ? d.requested : d.capture;
            if (!dev_.vertexCaptured(asked)) continue;
            DynBlas& b = dynBlas_[d.key];
            b.lastCapture = d.capture;
            b.lastSlots = d.slots;
            b.lastCaptureFrame = frame;
        }
        for (const DynDraw& d : dynPrev_) {
            if (!d.main || d.tree) continue;
            DynBlas& b = dynBlas_[d.key];
            if (b.lastCaptureFrame >= frame - 1 && b.lastCapture != 0) continue;
            const VkDeviceAddress asked = d.requested != 0 ? d.requested : d.capture;
            if (!dev_.vertexCapturedPrev(asked)) continue;
            b.lastCapture = d.capture;
            b.lastSlots = d.slots;
            b.lastCaptureFrame = frame - 1;
        }
    }
    if (dynUse_.empty()) return;
    vk::Context& ctx = dev_.ctx();
    constexpr VkBuildAccelerationStructureFlagsKHR kFlags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<VkDeviceSize> scratchOffsets;
    std::vector<std::size_t> which;
    std::vector<std::size_t> skippedRefits;
    geoms.reserve(dynUse_.size());
    infos.reserve(dynUse_.size());
    ranges.reserve(dynUse_.size());
    VkDeviceSize scratchTotal = 0;
    for (std::size_t i = 0; i < dynUse_.size(); ++i) {
        DynDraw& d = dynUse_[i];
        DynBlas& b = dynBlas_[d.key];
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& t = g.geometry.triangles;
        t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = d.capture + 16;
        t.vertexStride = 32;
        t.maxVertex = d.slots - 1;
        t.indexType = d.index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        t.indexData.deviceAddress = d.indices;
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = kFlags;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &g;
        if (b.sizeTriangles != d.triangles || b.sizeSlots != d.slots || b.sizeIndex32 != d.index32 || b.sizeAs == 0) {
            VkAccelerationStructureBuildSizesInfoKHR asked{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &d.triangles, &asked);
            b.sizeTriangles = d.triangles;
            b.sizeSlots = d.slots;
            b.sizeIndex32 = d.index32;
            b.sizeAs = asked.accelerationStructureSize;
            b.sizeBuildScratch = asked.buildScratchSize;
            b.sizeUpdateScratch = asked.updateScratchSize;
        }
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        sizes.accelerationStructureSize = b.sizeAs;
        sizes.buildScratchSize = b.sizeBuildScratch;
        sizes.updateScratchSize = b.sizeUpdateScratch;
        const bool update = b.built && b.slots == d.slots && b.triangles == d.triangles && b.indices == d.indices && b.updates < 30 &&
                            b.as.handle != VK_NULL_HANDLE;
        // the wind moves slowly: a tree's BLAS is refitted every other frame (staggered by its key), the BLAS of the frame before
        // stands in (its instance still takes the capture of this frame for the materials' vertex reads); a refit costs the
        // driver about a microsecond of command recording each, 100 trees in range are the bulk of the pass
        if (update && d.tree && ((frame + (d.key >> 7)) & 1u) != 0) {
            skippedRefits.push_back(i);
            continue;
        }
        if (!update) {
            if (b.as.handle == VK_NULL_HANDLE || b.as.storage.size < sizes.accelerationStructureSize) {
                if (b.as.handle != VK_NULL_HANDLE || b.as.storage.buffer != VK_NULL_HANDLE) {
                    vk::Context* c = &ctx;
                    vk::rt::AccelerationStructure old = b.as;
                    dev_.frames().retire([c, old]() mutable { old.destroy(*c); });
                    b.as = {};
                }
                if (!createAs(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, b.as, "rt scene character")) {
                    b.built = false;
                    continue;
                }
            }
            b.updates = 0;
        } else {
            ++b.updates;
        }
        info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = update ? b.as.handle : VK_NULL_HANDLE;
        info.dstAccelerationStructure = b.as.handle;
        const VkDeviceSize need = update ? sizes.updateScratchSize : sizes.buildScratchSize;
        scratchOffsets.push_back(scratchTotal);
        scratchTotal += (need + scratchAlign_ - 1) & ~(scratchAlign_ - 1);
        geoms.push_back(g);
        info.pGeometries = &geoms.back();
        infos.push_back(info);
        ranges.push_back({d.triangles, 0, 0, 0});
        which.push_back(i);
        b.slots = d.slots;
        b.triangles = d.triangles;
        b.indices = d.indices;
        b.lastFrame = frame;
        (update ? dynRefit_ : dynBuilt_) += 1;
    }
    for (std::size_t i : skippedRefits) {
        dynUse_[i].built = true;
        animatedKeys_.insert(dynUse_[i].key);
    }
    if (infos.empty()) return;
    if (dynScratch_.buffer == VK_NULL_HANDLE || dynScratch_.size < scratchTotal) {
        if (dynScratch_.buffer != VK_NULL_HANDLE) {
            vk::Context* c = &ctx;
            vk::rt::AddressBuffer old = dynScratch_;
            dev_.frames().retire([c, old]() mutable { old.destroy(*c); });
            dynScratch_ = {};
        }
        if (!dynScratch_.create(ctx, std::max<VkDeviceSize>(scratchTotal * 2, 4ull << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratchAlign_,
                                "rt scene character scratch")) {
            for (std::size_t i : which) dynBlas_[dynUse_[i].key].built = false;
            return;
        }
    }
    for (std::size_t k = 0; k < infos.size(); ++k) infos[k].scratchData.deviceAddress = dynScratch_.address + scratchOffsets[k];
    // the vertex shaders' captures (the cascades' and the solid pass's draws) and the skinning pass's output -> the builds
    barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pr(ranges.size());
    for (std::size_t k = 0; k < ranges.size(); ++k) pr[k] = &ranges[k];
    {
        d3d9::RegionScope r(dev_, "rt.dynamic builds");
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<std::uint32_t>(infos.size()), infos.data(), pr.data());
    }
    for (std::size_t i : which) {
        dynUse_[i].built = true;
        dynBlas_[dynUse_[i].key].built = true;
        if (dynUse_[i].tree) animatedKeys_.insert(dynUse_[i].key);
    }
}

// ---- characters skinned by compute ----------------------------------------------------------------------------------------------

void RtScene::setGpuSkinning(bool on, bool verify) {
    skinWanted_ = on;
    skinVerify_ = on && verify;
    if (!on) skinJobs_.clear();
}

// the pipeline builds in the background on first use; until it is ready the characters take the vertex shader captures
bool RtScene::skinReady() {
    if (!skinWanted_ || skinFailed_ || !dynamicSupported()) return false;
    if (skinReady_) return true;
    if (!skinStarted_) {
        skinStarted_ = true;
        if (!skinLayout_.create(dev_.ctx(), dev_.bindless(), {})) {
            skinFailed_ = true;
            return false;
        }
        vk::Context* ctx = &dev_.ctx();
        const VkPipelineLayout layout = skinLayout_.layout();
        skinBuild_ = std::async(std::launch::async, [ctx, layout]() {
            return vk::createComputePipeline(*ctx, layout, shader::embedded::spirv("rt/rt_skin.comp"), "rt.skin");
        });
        return false;
    }
    if (!skinBuild_.valid() || skinBuild_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    skinPipe_ = skinBuild_.get();
    if (skinPipe_ == VK_NULL_HANDLE) {
        spdlog::error("rt: skin: the compute pipeline failed: characters keep the vertex shader captures");
        skinFailed_ = true;
        return false;
    }
    skinReady_ = true;
    spdlog::info("rt: skin: characters skinned by compute (bone matrices of the draws' vertex shader constants){}", skinVerify_ ? ", verifying against the capture variant" : "");
    return true;
}

void RtScene::destroySkin() {
    skinJobs_.clear();
    vk::Context* ctx = &dev_.ctx();
    if (skinBuild_.valid()) {
        const VkPipeline p = skinBuild_.get();
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
    }
    const VkPipeline pipe = skinPipe_;
    vk::PassLayout layout = skinLayout_;
    vk::rt::AddressBuffer err = skinErr_;
    if (pipe != VK_NULL_HANDLE || layout.valid() || err.buffer != VK_NULL_HANDLE) {
        dev_.frames().retire([ctx, pipe, layout, err]() mutable {
            if (pipe != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), pipe, nullptr);
            layout.destroy(*ctx);
            err.destroy(*ctx);
        });
    }
    skinPipe_ = VK_NULL_HANDLE;
    skinLayout_ = {};
    skinErr_ = {};
    skinReady_ = skinStarted_ = false;
}

// one dispatch for all of the frame's jobs (the frame's first skinned draws: z pre-pass and cascades), before the BLAS builds
void RtScene::recordSkin(VkCommandBuffer cmd) {
    if (skinJobs_.empty()) return;
    if (!skinReady()) {
        skinJobs_.clear();
        return;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(skinJobs_.size(), 512));
    std::uint32_t chunks = 0;
    for (std::uint32_t i = 0; i < n; ++i) chunks += (skinJobs_[i].slots + 63) / 64;
    const vk::RingAlloc t = dev_.frames().allocate(16 + 64ull * n, 16);
    if (t.ptr == nullptr) {
        skinJobs_.clear();
        return;
    }
    VkDeviceAddress errAddress = 0;
    const std::uint32_t slot = dev_.frames().currentSlot();
    if (skinVerify_) {
        if (skinErr_.buffer == VK_NULL_HANDLE &&
            !skinErr_.create(dev_.ctx(), 16ull * vk::Frames::kInFlight, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, 16, "rt skin verify")) {
            skinVerify_ = false;
        } else if (skinErr_.mapped != nullptr) {
            auto* e = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(skinErr_.mapped) + 16ull * slot);
            // this slot's previous frame has completed (Frames::begin waited for it)
            if (e[1] != 0 || e[2] != 0) {
                float maxErr = 0.0f;
                std::memcpy(&maxErr, &e[0], 4);
                skinVerifyCompared_ += e[1];
                skinVerifyMissing_ += e[2];
                skinVerifyMax_ = std::max(skinVerifyMax_, maxErr);
                const double now = dev_.secondsSinceCreate();
                if (now >= skinVerifyAt_ + 1.0) {
                    spdlog::info("rt: skin verify: {} vertices compared with the vertex shader's capture, max error {:.4f} units, {} slots the reference did not write",
                                 skinVerifyCompared_, skinVerifyMax_, skinVerifyMissing_);
                    skinVerifyCompared_ = skinVerifyMissing_ = 0;
                    skinVerifyMax_ = 0.0f;
                    skinVerifyAt_ = now;
                }
            }
            e[0] = e[1] = e[2] = e[3] = 0;
            errAddress = skinErr_.address + 16ull * slot;
        }
    }
    auto* head = static_cast<std::uint32_t*>(t.ptr);
    head[0] = n;
    head[1] = chunks;
    head[2] = static_cast<std::uint32_t>(errAddress);
    head[3] = static_cast<std::uint32_t>(errAddress >> 32);
    std::uint32_t* jobs = head + 4;
    std::uint32_t chunk = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const SkinJob& j = skinJobs_[i];
        std::uint32_t* w = jobs + 16ull * i;
        w[0] = static_cast<std::uint32_t>(j.vertices);
        w[1] = static_cast<std::uint32_t>(j.vertices >> 32);
        w[2] = static_cast<std::uint32_t>(j.bones);
        w[3] = static_cast<std::uint32_t>(j.bones >> 32);
        w[4] = static_cast<std::uint32_t>(j.out);
        w[5] = static_cast<std::uint32_t>(j.out >> 32);
        w[6] = j.slots;
        w[7] = j.stride;
        w[8] = chunk;
        w[9] = 0;
        w[10] = static_cast<std::uint32_t>(j.ref);
        w[11] = static_cast<std::uint32_t>(j.ref >> 32);
        w[12] = j.offsets;
        w[13] = w[14] = w[15] = 0;
        chunk += (j.slots + 63) / 64;
        skinVerticesTotal_ += j.slots;
    }
    {
        d3d9::RegionScope r(dev_, "rt.skin");
        // a reference capture written by a cascade's vertex shader (verify) before the compute reads it
        barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skinPipe_);
        skinLayout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dev_.bindless());
        struct Push {
            abi::PassPushHeader h;
            std::uint32_t table[2];
            std::uint32_t pad[2];
        } push{};
        push.h.frame = 0;
        push.table[0] = static_cast<std::uint32_t>(t.address);
        push.table[1] = static_cast<std::uint32_t>(t.address >> 32);
        skinLayout_.push(cmd, &push, sizeof(push));
        vkCmdDispatch(cmd, chunks, 1, 1);
    }
    for (std::uint32_t i = 0; i < n; ++i) dev_.markVertexCaptured(skinJobs_[i].out);
    skinJobsTotal_ += n;
    skinJobs_.clear();
    const double now = dev_.secondsSinceCreate();
    if (now >= skinLogAt_ + 5.0) {
        skinLogAt_ = now;
        spdlog::info("rt: skin: {} draws / {} vertices skinned by compute so far ({} this frame)", skinJobsTotal_, skinVerticesTotal_, n);
    }
}

// characters not seen for 2 s (all: off / deactivated): BLAS + material released
void RtScene::dropDynamic(bool all, std::uint64_t frame) {
    vk::Context* ctx = &dev_.ctx();
    for (auto it = dynBlas_.begin(); it != dynBlas_.end();) {
        DynBlas& b = it->second;
        if (!all && b.lastFrame + 120 >= frame) {
            ++it;
            continue;
        }
        if (b.material != 0) materials_.release(b.material);
        if (b.as.handle != VK_NULL_HANDLE || b.as.storage.buffer != VK_NULL_HANDLE) {
            vk::rt::AccelerationStructure old = b.as;
            dev_.frames().retire([ctx, old]() mutable { old.destroy(*ctx); });
        }
        it = dynBlas_.erase(it);
    }
    if (all) {
        dyn_.clear();
        dynPrev_.clear();
        dynUse_.clear();
        if (dynScratch_.buffer != VK_NULL_HANDLE) {
            vk::rt::AddressBuffer old = dynScratch_;
            dev_.frames().retire([ctx, old]() mutable { old.destroy(*ctx); });
            dynScratch_ = {};
        }
    }
}

// ---- the pass ----------------------------------------------------------------------------------------------------------------

void RtScene::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !active_ || failed_) return;
    if (builtFrame_ == dev_.frameValue()) return;  // built at the z pre-pass already
    builtFrame_ = dev_.frameValue();
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
    if (frame % 300 == 0) {
        std::erase_if(blendedKeys_, [&](const auto& e) { return e.second + kOpaqueRecent < frame; });
        std::erase_if(layers_, [&](auto& e) {
            if (e.second.seen + kOpaqueRecent >= frame) return false;
            if (e.second.material != 0) materials_.release(e.second.material);
            return true;
        });
    }
    {
        d3d9::RegionScope r(dev_, "rt.compact");
        processCompactions(cmd, slot);
    }
    {
        d3d9::RegionScope r(dev_, "rt.blas builds");
        buildPending(cmd, slot);
    }
    {
        d3d9::RegionScope r(dev_, "rt.dynamic");
        buildDynamic(cmd, frame);
    }
    // BLAS builds / compaction copies -> the TLAS build
    barrier(cmd, asStages, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    // characters built this frame (setDynamic) after the world's instances
    std::uint32_t dynCount = 0;
    for (const DynDraw& d : dynUse_) dynCount += d.built ? 1u : 0u;
    dynCount = std::min(dynCount, tlas_[slot].capacity / 8);
    // the instances and their geometry records (one geometry per BLAS: record index = instanceCustomIndex)
    const auto& list = registry_.instances();
    if (frame % 16 == 0) {
        // the registry's ghosts of volatile ranges (the TLAS skips them anyway): dropped
        registry_.removeIf([&](const rt::Registry::Instance& in) {
            const Blas* b = findBlas(in.blas);
            return b != nullptr && b->volatileUntil >= frame && in.lastFrame + 4 * kVolatileKeep < frame;
        });
    }
    const std::vector<std::uint32_t> chosen = rt::selectForTlas(list, camera, tlas_[slot].capacity - dynCount, [this, frame](const rt::Registry::Instance& in) {
        const Blas* b = findBlas(in.blas);
        // an object the cascades draw at another level of detail than the main view: the main view's
        if (b == nullptr || b->state != BlasState::Built || registry_.supersededBy(in, viewBit(View::Main))) return false;
        if (b->animatedOnly) return false;
        // an animated SpeedTree stands in for its rest-pose instance (the wind: observeTree)
        if (!animatedKeys_.empty() && animatedKeys_.contains(in.key)) return false;
        // volatile ranges (churning instances): only what was seen in the last frames, the stale ones are ghosts of earlier places
        if (b->volatileUntil >= frame && in.lastFrame + kVolatileKeep < frame) return false;
        // an instance known only through an overlay material (caustics, a blended untextured wet-sand strip) lies on another
        // surface: left out (its coplanar hits would speckle the base)
        if (in.material != 0) {
            for (const Blas::MaterialSlot& m : b->materials) {
                if (m.id == in.material) return !m.overlay;
            }
        }
        // no own material: the BLAS's best, unless it has only overlays
        return b->materials.empty() || std::any_of(b->materials.begin(), b->materials.end(), [](const auto& m) { return !m.overlay; });
    });
    const std::uint32_t worldCount = std::min<std::uint32_t>(static_cast<std::uint32_t>(chosen.size()), tlas_[slot].capacity - dynCount);
    const std::uint32_t n = worldCount + dynCount;
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
    std::uint64_t textured = 0, untextured = 0, cutOut = 0, segmented = 0, speedTrees = 0;
    std::vector<abi::RtMaterialSegment> segs;              // the frame's segment table
    std::vector<std::pair<std::uint32_t, std::uint32_t>> segRecords;  // record -> its first segment
    for (std::uint32_t k = 0; k < worldCount; ++k) {
        const rt::Registry::Instance& in = list[chosen[k]];
        Blas& b = *findBlas(in.blas);
        // the material (M9 P2): the bindless slots of this frame (a texture that died is gone from the entry)
        const RtMaterial* m = in.material != 0 ? materials_.get(in.material) : nullptr;
        if (m == nullptr || m->dead) m = bestMaterial(b);
        const MaterialFields mf = materialFields(m);
        const std::uint32_t albedoTex = mf.albedoTex;
        const std::uint32_t opacityTex = mf.opacityTex;
        // alpha-tested (the draw's alpha test, fronds, a masked material) and something to test against: non-opaque
        const bool wantsCut = (in.flags & abi::kRtRecordAlphaTested) != 0 || (m != nullptr && m->masked);
        const bool alpha = wantsCut && m != nullptr && opacityTex != 0 && (m->uvSet & abi::kRtUvOpacityOpaque) == 0;
        (albedoTex != 0 ? textured : untextured) += 1;
        speedTrees += (in.flags & abi::kRtRecordSpeedTree) != 0 ? 1 : 0;
        cutOut += alpha ? 1 : 0;
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
        split(key.strip != 0 ? b.listIndices.address : b.ibAddress + static_cast<VkDeviceSize>(key.first) * (key.index32 != 0 ? 4u : 2u),
              r.indices);
        r.stride = key.stride;
        r.flags = (in.flags & ~abi::kRtRecordAlphaTested) | (alpha ? abi::kRtRecordAlphaTested : 0u) |
                  (((in.data >> 16) & 0xFFu) << abi::kRtRecordKindShift);
        r.normal = b.layout.normal;
        r.texcoord0 = b.layout.texcoord0;
        r.texcoord1 = b.layout.texcoord1;
        r.texcoord6 = b.layout.texcoord6;
        r.color0 = b.layout.color0;
        r.instanceId = in.id;
        // the draw's alpha test, else the masked material's own (GREATER MaskReference), else D3DCMP_ALWAYS
        r.alpha = (in.data & 0xFFFFu) != 0 ? (in.data & 0xFFFFu) : (m != nullptr && m->alpha != 0) ? m->alpha : 8u;
        r.albedoTex = mf.albedoTex;
        r.albedoSampler = mf.albedoSampler;
        r.uvSet = mf.uvSet;
        std::memcpy(r.uvMap, mf.uvMap, sizeof(r.uvMap));
        // a range without a main-view material of its own (a cascade's merged level-mesh range): per-triangle materials from
        // the main view's sub-ranges inside it
        const bool strong = std::any_of(b.materials.begin(), b.materials.end(), [](const auto& ms) { return ms.rank >= 3 && !ms.overlay; });
        if (!strong && b.key.primitives >= 64 && b.key.strip == 0) {
            if (b.segmentsFrame == 0 || frame >= b.segmentsFrame + 60) {
                refreshSegments(b);
                b.segmentsFrame = frame;
            }
            if (!b.segments.empty()) {
                segRecords.emplace_back(k, static_cast<std::uint32_t>(segs.size()));
                for (const Blas::Segment& seg : b.segments) {
                    const Blas* sub = seg.blas != 0 ? findBlas(seg.blas) : nullptr;
                    const RtMaterial* sm = sub != nullptr ? bestMaterial(*sub) : seg.material != 0 ? materials_.get(seg.material) : nullptr;
                    const MaterialFields sf = materialFields(sm);
                    abi::RtMaterialSegment e{};
                    e.firstPrimitive = seg.firstPrimitive;
                    e.primitiveCount = seg.primitiveCount;
                    e.albedoTex = sf.albedoTex;
                    e.albedoSampler = sf.albedoSampler;
                    e.alpha = r.alpha;
                    e.uvSet = sf.uvSet;
                    std::memcpy(e.uvMap, sf.uvMap, sizeof(e.uvMap));
                    segs.push_back(e);
                }
                r.segmentCount = static_cast<std::uint32_t>(b.segments.size());
                r.flags |= abi::kRtRecordSegmented;
                ++segmented;
            }
        }
        recs[k] = r;
        triangles += b.triangles;
        if (content != nullptr) content->push_back({in.blas, in.id, r.flags, in.material, {in.rows[3], in.rows[7], in.rows[11]}});
    }

    // the characters: world-space vertices (identity transform), no normals, the draw's diffuse material
    // `used`: the instances written (an animated tree or character can drop out below: its registry instance or BLAS is gone, so
    // fewer than the n counted above). The TLAS must be built from exactly these: the ring's tail would hold the instances of
    // earlier frames, with references to acceleration structures that may be destroyed by now (a GPU hang in the TLAS build).
    std::uint32_t used = worldCount;
    {
        std::uint32_t k = worldCount;
        std::uint32_t treesNow = 0;
        for (const DynDraw& d : dynUse_) {
            if (k >= n) break;
            if (!d.built) continue;
            const auto it = dynBlas_.find(d.key);
            if (it == dynBlas_.end() || !it->second.built) continue;
            const DynBlas& b = it->second;
            VkAccelerationStructureInstanceKHR vi{};
            vi.transform.matrix[0][0] = vi.transform.matrix[1][1] = vi.transform.matrix[2][2] = 1.0f;
            vi.instanceCustomIndex = k;
            vi.mask = abi::kRtMaskDynamic;
            vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            vi.accelerationStructureReference = b.as.address;
            if (d.tree) {
                // an animated SpeedTree: the rest-pose instance's record (its material, alpha test, flags) with the captured
                // world-space vertices; a world instance for the masks (not a character)
                const Blas* sb = findBlas(d.blas);
                const rt::Registry::Instance* in = registry_.find(d.key);
                if (sb == nullptr || in == nullptr) continue;
                const RtMaterial* m = in->material != 0 ? materials_.get(in->material) : nullptr;
                if (m == nullptr || m->dead) m = bestMaterial(*sb);
                const MaterialFields mf = materialFields(m);
                const bool wantsCut = (in->flags & abi::kRtRecordAlphaTested) != 0 || (m != nullptr && m->masked);
                const bool alpha = wantsCut && m != nullptr && mf.opacityTex != 0 && (m->uvSet & abi::kRtUvOpacityOpaque) == 0;
                vi.mask = alpha ? abi::kRtMaskAlphaTested : abi::kRtMaskOpaque;
                vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                           (alpha ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
                insts[k] = vi;
                abi::RtGeometryRecord r{};
                split(d.capture + 16, r.vertices);
                split(d.indices, r.indices);
                r.stride = 32;
                r.flags = (in->flags & ~abi::kRtRecordAlphaTested) | (alpha ? abi::kRtRecordAlphaTested : 0u) | (((in->data >> 16) & 0xFFu) << abi::kRtRecordKindShift);
                r.normal = abi::kRtAttribAbsent;
                r.texcoord0 = 16u | (1u << 16);  // FLOAT2 at 16: the capture's TEXCOORD0
                r.texcoord1 = r.texcoord0;
                r.texcoord6 = abi::kRtAttribAbsent;
                r.color0 = abi::kRtAttribAbsent;
                r.instanceId = in->id;
                r.alpha = (in->data & 0xFFFFu) != 0 ? (in->data & 0xFFFFu) : (m != nullptr && m->alpha != 0) ? m->alpha : 8u;
                r.albedoTex = mf.albedoTex;
                r.albedoSampler = mf.albedoSampler;
                r.uvSet = mf.uvSet;
                std::memcpy(r.uvMap, mf.uvMap, sizeof(r.uvMap));
                recs[k] = r;
                triangles += d.triangles;
                ++treesNow;
                if (content != nullptr) content->push_back({d.blas, in->id, r.flags, in->material, {in->rows[3], in->rows[7], in->rows[11]}});
                ++k;
                continue;
            }
            insts[k] = vi;
            abi::RtGeometryRecord r{};
            split(d.capture + 16, r.vertices);
            split(d.indices, r.indices);
            r.stride = 32;
            r.flags = (d.index32 ? abi::kRtRecordIndex32 : 0u) | abi::kRtRecordDynamic | (static_cast<std::uint32_t>(Kind::Skinned) << abi::kRtRecordKindShift) |
                      (1u << abi::kRtRecordViewShift);
            r.normal = abi::kRtAttribAbsent;
            r.texcoord0 = 16u | (1u << 16);  // FLOAT2 at 16: the capture's TEXCOORD0
            r.texcoord1 = r.texcoord0;
            r.texcoord6 = abi::kRtAttribAbsent;
            r.color0 = abi::kRtAttribAbsent;
            r.instanceId = b.id;
            r.alpha = 8u;
            const RtMaterial* m = b.material != 0 ? materials_.get(b.material) : nullptr;
            const MaterialFields mf = materialFields(m != nullptr && !m->dead ? m : nullptr);
            r.albedoTex = mf.albedoTex;
            r.albedoSampler = mf.albedoSampler;
            r.uvSet = mf.uvSet | abi::kRtUvOpacityOpaque;
            std::memcpy(r.uvMap, mf.uvMap, sizeof(r.uvMap));
            recs[k] = r;
            triangles += d.triangles;
            if (content != nullptr) content->push_back({0, b.id, r.flags, b.material, {0.0f, 0.0f, 0.0f}});
            ++k;
        }
        iv_.dynamic = k - worldCount;
        iv_.animatedTrees = treesNow;
        used = k;
    }

    // the segment table (after the records: their addresses)
    if (!segs.empty()) {
        const vk::RingAlloc sa = frames.allocate(segs.size() * sizeof(abi::RtMaterialSegment), 16);
        if (sa.ptr != nullptr) {
            std::memcpy(sa.ptr, segs.data(), segs.size() * sizeof(abi::RtMaterialSegment));
            for (const auto& [rec, first] : segRecords) split(sa.address + static_cast<VkDeviceSize>(first) * sizeof(abi::RtMaterialSegment), recs[rec].segments);
        } else {
            for (const auto& [rec, first] : segRecords) {
                recs[rec].segmentCount = 0;
                recs[rec].flags &= ~abi::kRtRecordSegmented;
            }
        }
    }
    iv_.segmented = segmented;
    iv_.speedTree = speedTrees;
    iv_.segments = segs.size();

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
    const VkAccelerationStructureBuildRangeInfoKHR range{used, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
    {
        d3d9::RegionScope r(dev_, "rt.tlas build");
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &pr);
    }
    // the TLAS -> ray queries in fragment / compute shaders of this submission
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, rayStages,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    if (timePool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timePool_, slot * 2 + 1);
        timeWritten_[slot] = true;
    }
    if (used < n) {
        // (see `used`) counted for the log; before this was fixed the TLAS was built over n instances, the tail of them stale
        // ring memory: VK_ERROR_DEVICE_LOST after a teleport with shadows.rt on (docs/research/rt-shadows.md 9)
        ++iv_.holeFrames;
        iv_.holes += n - used;
    }
    dev_.setRtScene({t.as.address, ra.address, used, used, frame});

    lastInstances_ = used;
    lastTriangles_ = triangles;
    iv_.textured = textured;
    iv_.untextured = untextured;
    iv_.cutOut = cutOut;
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
    const bool statusDue = iv_.frames % 30 == 1;
    if (!due && !force && !statusDue) return;  // the counts below walk every BLAS: only when they are shown
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
                 "skipped, {:.0f} instance sightings), build CPU {:.3f} ms, pass CPU {:.3f} ms, GPU {}",
                 lastInstances_, registry_.size(), c.added, c.replaced, c.aged, c.tooFar, c.capped, c.dropped, built, iv_.built,
                 iv_.builtTriangles / 1000, pendingCount, failed, iv_.compacted, static_cast<double>(iv_.compactSaved) / 1048576.0, iv_.evicted,
                 lastTriangles_, blasMiB + tlasMiB + scratchMiB, blasMiB, tlasMiB, scratchMiB, iv_.observeMs / f,
                 static_cast<double>(iv_.observed) / f, static_cast<double>(iv_.skipped) / f, static_cast<double>(iv_.sightings) / f, iv_.buildCpuMs / f, iv_.passCpuMs / f,
                 iv_.gpuSamples > 0 ? std::format("{:.3f} ms", iv_.gpuMs / static_cast<double>(iv_.gpuSamples)) : std::string("n/a ([debug] perf)"));
    const RtMaterials::Counters mc = materials_.takeCounters();
    spdlog::info("rt: scene: materials {} (per frame: {:.1f} resolves, {:.3f} ms; interval: {} uv evaluations, graph {} (pool image {}) / bytecode s9 {} / "
                 "no texture {}, approximate {}, failed {}, active shader != material's {}); records: textured {}, untextured {}, cut-out {}, segmented {} ({} segments; "
                 "engine sub-mesh segments {}, merged ranges resolved {}), SpeedTree {}",
                 materials_.size(), static_cast<double>(mc.resolves) / f, mc.ms / f, mc.evaluations, mc.graph, mc.pool, mc.heuristic, mc.noTexture,
                 mc.approx, mc.failed, mc.shaderMismatch, iv_.textured, iv_.untextured, iv_.cutOut, iv_.segmented, iv_.segments,
                 iv_.engineSegments, iv_.engineMerged, iv_.speedTree);
    if (dynamic_) {
        spdlog::info("rt: scene: characters {} + animated SpeedTrees {} in the last TLAS ({} tracked, interval: {} BLAS builds, {} refits; "
                     "volatile ranges {}; tree draws {} ({} beyond the range, {} captured); BLAS refits from this frame's capture {} / the previous frame's {}; characters: {} from this frame's pose ({} skinned by compute) / {} from the previous frame's (skinning pass: {} draws taken, {} not eligible; solid-pass draws {} with / {} without a job); capture variant needs vertexPipelineStoresAndAtomics: {})",
                     iv_.dynamic - iv_.animatedTrees, iv_.animatedTrees, dynBlas_.size(), dynBuilt_, dynRefit_, iv_.volatileBlas, iv_.treeDraws, iv_.treeFar, iv_.treeCaptured, iv_.capCurrent, iv_.capPrevious,
                     iv_.charCurrent, iv_.charSkinned, iv_.charPrevious, iv_.skinDraws, iv_.skinIneligible, iv_.solidWithSkin, iv_.solidNoSkin, dev_.ctx().features().vertexStores ? "yes" : "no");
        dynBuilt_ = dynRefit_ = 0;
    }
    if (iv_.holes > 0) {
        spdlog::info("rt: scene: TLAS built over fewer instances than counted in {} frames of the interval ({} dynamic instances dropped out: their registry instance or BLAS was gone)",
                     iv_.holeFrames, iv_.holes);
    }
    iv_ = {};
    lastLog_ = now;
}

}  // namespace rvk::enh
