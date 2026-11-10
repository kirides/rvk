#include "enh/rt_scene.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "engine/materials.h"
#include "engine/scene_table.h"
#include "enh/material_variant.h"
#include "enh/rhi_scope.h"
#include "shader/embedded.h"
#include "engine/memread.h"
#include "shader/pass_push.h"

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
constexpr float kBoxMaxRadius = 2000.0f;        // world units (20 m): bigger objects (cliffs, level meshes) never become boxes
constexpr std::uint32_t kBoxMinTriangles = 64;  // a smaller BLAS gains nothing from a 12-triangle box
constexpr float kBoxShrink = 0.92f;             // the box's half extents relative to the object's bounds
constexpr float kTreeThin = 0.45f;              // a tree's box: the two short axes relative to the shrunk bounds
constexpr std::uint64_t kOpaqueRecent = 600;               // frames: "the main view draws this range opaque"
constexpr std::uint64_t kMaterialRefresh = 64;             // frames: a BLAS's material is resolved again (animated uv maps)
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

void split(rhi::GpuRef a, std::uint32_t (&out)[2]) {
    out[0] = a.low();
    out[1] = a.high();
}

// a buffer's location in both forms: the shader reference (records, push data) and the raw address (acceleration structure geometry)
rhi::GpuAddr addrOf(const rhi::Buffer& b, std::uint64_t offset = 0) {
    const std::array<std::uint32_t, 2> r = b.shaderRef(offset);
    return {rhi::GpuRef::fromWords(r[0], r[1]), b.gpuAddress() + offset};
}

// The alpha test of a geometry record (func | ref << 8, D3DCMP_*). SpeedTrees: the engine's cut-out is GREATER against the
// shader's MaskReference, 80 for every SpeedTree material (rt-albedo-facts.md 2); it is done by `texkill` in the z / shadow
// passes, so a draw's own alpha-test state (the last sighting's, any pass) says nothing about it. Others: the draw's state,
// else the masked material's own (GREATER MaskReference), else D3DCMP_ALWAYS.
constexpr std::uint32_t kSpeedTreeAlpha = 5u | (80u << 8);
std::uint32_t recordAlpha(std::uint32_t flags, std::uint32_t data, const RtMaterial* m) {
    if ((flags & abi::kRtRecordSpeedTree) != 0 && (flags & abi::kRtRecordAlphaTested) != 0) return kSpeedTreeAlpha;
    if ((data & 0xFFFFu) != 0) return data & 0xFFFFu;
    return m != nullptr && m->alpha != 0 ? m->alpha : 8u;
}

}  // namespace

RtScene::RtScene(d3d9::Device& device) : dev_(device) {
    registry_.onAdded = [this](const rt::Registry::Instance& in) {
        if (Blas* b = findBlas(in.blas)) ++b->refs;
    };
    registry_.onRemoved = [this](const rt::Registry::Instance& in) {
        if (Blas* b = findBlas(in.blas); b != nullptr && b->refs > 0) --b->refs;
        if (in.pinned) tableInstanceRemoved(in.key);
    };
}

RtScene::~RtScene() {
    consumers_.clear();
    deactivate();
}

bool RtScene::supported() const { return dev_.game().gameCaps().rayTracing; }

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
    tableClear();
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
    // (a window of the pack's glass entries is drawn in the refracted pass like water, but it is no water: kept as a glass pane while a glass consumer exists)
    const bool mainBlended = q.phase.view == View::Main && q.blending;
    const bool realWater = q.tag != nullptr && q.tag->water != d3d9::draws::Water::None;
    const bool glassDraw = q.phase.view == View::Main && (q.blending || q.kind == Kind::Refracted) && !realWater && isGlassDraw(q);
    const bool water = !glassDraw && (q.kind == Kind::Refracted || realWater);
    if (water || (mainBlended && !glassDraw)) {
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
    if (q.phase.view == View::Main && (q.phase.pass == Pass::Alpha || q.phase.pass == Pass::AlphaLate) && !glassDraw) return skip();
    if (q.tag != nullptr && q.tag->material != nullptr && waterMaterials_.contains(q.tag->material)) return skip();
    if (q.blending && !glassDraw) return reject("blended");                          // alpha-blended: not P1
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
    const rhi::GpuAddr vbAddress = vb->gpuAddress(), ibAddress = ib->gpuAddress();
    if (vbAddress.null() || ibAddress.null()) return skip();
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
    if (glassDraw && !b->glass) {
        b->glass = true;
        spdlog::info("rt: scene: glass pane BLAS #{}: material '{}'", b->id, q.tag != nullptr && q.tag->material != nullptr ? engine::materials::resourceName(q.tag->material) : "?");
    }
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
    // SpeedTree fronds (op 1), leaf cards and leaf meshes (op > 1) are cut-outs; branches (op 0) are not unless the draw says so
    const bool alphaTested = q.alphaTest || (speedTree && q.tag->opDesc >= 1);
    const std::uint32_t flags = (key.index32 != 0 ? abi::kRtRecordIndex32 : 0u) | (alphaTested ? abi::kRtRecordAlphaTested : 0u) |
                                (q.depthBias && q.phase.view == View::Main ? abi::kRtRecordDepthBias : 0u) |
                                (speedTree ? abi::kRtRecordSpeedTree : 0u) | (b->glass ? abi::kRtRecordGlass : 0u) | (q.instanced ? abi::kRtRecordInstanced : 0u) |
                                (!q.depthWrite && q.phase.view == View::Main && q.phase.pass == Pass::Solid ? abi::kRtRecordNoDepthWrite : 0u) |
                                (static_cast<std::uint32_t>(viewBit(q.phase.view)) << abi::kRtRecordViewShift);
    // the instance's data word: the alpha test (func | ref << 8, 0 = none) and the draw kind << 16 (the latest sighting's;
    // flags are OR-ed over sightings, a kind must not be)
    const std::uint32_t alphaWord = (q.alphaTest ? (q.alphaFunc | (q.alphaRef << 8)) : 0u) | (static_cast<std::uint32_t>(q.kind) << 16);
    const std::uint8_t view = viewBit(q.phase.view);
    const auto rowsAt = [&](std::uint32_t reg, rt::Rows& rows) {
        // F2c-2: the world rows of a draw with an object-table window come from the table (3 float4: c33..c35), not the register file
        if (q.objectRows != nullptr && !q.instanced && reg == kInstanceDataBase + 1) {
            for (std::uint32_t r = 0; r < 3; ++r) std::memcpy(&rows[r * 4], q.objectRows + 4 * r, sizeof(float) * 4);
            return rt::finite(rows);
        }
        for (std::uint32_t r = 0; r < 3; ++r) std::memcpy(&rows[r * 4], dev.vsConstant(reg + r), sizeof(float) * 4);
        return rt::finite(rows);
    };
    if (!q.instanced) {
        rt::Rows rows;
        if (!rowsAt(kInstanceDataBase + 1, rows) || outside(rows)) return skip();
        const std::uint64_t owner = q.tag != nullptr ? rt::ownerKey(q.tag->entity, q.tag->opDesc) : 0;
        ++iv_.sightings;
        // F8-0 check frame: the instance as it stands before this sighting (what a covered draw would have left untouched)
        rt::Registry::Instance pre;
        bool havePre = false;
        if (tableOn_ && tableCompare_ && !speedTree && tableCompareFrame(frame)) {
            if (const rt::Registry::Instance* p = registry_.find(rt::instanceKey(b->id, rows))) {
                pre = *p;
                havePre = true;
            }
        }
        auto sr = registry_.see(rt::instanceKey(b->id, rows), b->id, rows, owner, flags, alphaWord, view, frame, now);
        rt::Registry::Instance* in = sr.first;
        if (sr.second) noteNewInstance(*b, frame);
        if (in != nullptr && materialId != 0) applyMaterial(*in, materialId, materialRank, overlay, frame);
        if (in != nullptr && tableOn_ && in->ambiguous) tableDropInstance(in->key);
        if (in != nullptr && tableOn_ && !speedTree) tableObserved(q, *b, *in, rows, sr.second, havePre ? &pre : nullptr);
        if (in != nullptr && speedTree && q.phase.view == View::Main && q.phase.pass == Pass::Solid) treeSetNote(dev, *in);
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
        if (in != nullptr && tableOn_ && in->ambiguous) tableDropInstance(in->key);
        if (in != nullptr && speedTree && q.phase.view == View::Main && q.phase.pass == Pass::Solid) treeSetNote(dev, *in);
        // the capture's instance k is the batch's k-th instance (the instance stream is 0, 1, 2, ...)
        if (in != nullptr && treeBatchOk && v == static_cast<float>(i)) observeTree(dev, q, *b, *in, rows, treeBatch, i, n);
    }
    if (!instanceIndexChecked_ && fromStream && iv_.observed > 2000) {
        instanceIndexChecked_ = true;
        spdlog::info("rt: scene: instance stream check: {}", indexDeviations_ == 0 ? "TEXCOORD4 = 0, 1, 2, ... in every batch so far"
                                                                                   : std::format("{} instances deviate from 0, 1, 2, ...", indexDeviations_));
    }
}

void RtScene::treeSetNote(const d3d9::Device& dev, const rt::Registry::Instance& in) {
    const std::uint64_t frame = dev.frameValue();
    if (treeSet_.frame != frame) {
        treeSetClose(dev);
        treeSet_ = {frame, 0, 0, false};
    }
    if (dev.ownedTreeObservedFrame() == frame) treeSet_.owned = true;
    ++treeSet_.count;
    std::uint64_t h = in.key * 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(in.flags) * 0xC2B2AE3D27D4EB4Full) ^ (static_cast<std::uint64_t>(in.data) * 0x165667B19E3779F9ull);
    h ^= h >> 29;
    treeSet_.hash += h;
}

void RtScene::treeSetClose(const d3d9::Device& dev) {
    if (treeSet_.frame == ~0ull || treeSet_.count == 0) return;
    (void)dev;
    const bool owned = treeSet_.owned;
    const std::uint64_t id = treeSet_.hash * 31 + treeSet_.count;
    ++treeSets_[owned ? 1 : 0][id];
    treeSet_.count = 0;
}

// ---- F8-0 persistent table entries (docs/research/frame-ownership.md 7.20) ------------------------------------------------------

namespace {

std::uint64_t tableMix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 31);
}

// the 3 float4 world rows of the draw (VS c33..c35) from an entity's matrix (entity +0x60, 4x4): row r = columns 0..2 of the matrix (as the object table writes them)
void rowsOfWorld(const float* m, rt::Rows& rows) {
    for (int r = 0; r < 3; ++r) {
        for (int j = 0; j < 4; ++j) rows[static_cast<std::size_t>(r * 4 + j)] = m[4 * j + r];
    }
}

bool rowsNear(const rt::Rows& a, const rt::Rows& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i] - b[i]) > 1e-5f + 1e-6f * std::fabs(a[i])) return false;
    }
    return true;
}

}  // namespace

std::uint8_t RtScene::tableClass(d3d9::scene::Phase p) {
    if (p.view == View::Shadow) return 4;
    if (p.view != View::Main) return 0;
    return p.pass == Pass::Solid ? 1 : p.pass == Pass::DepthPrepass ? 2 : 0;
}

std::uint64_t RtScene::tableKey(const void* entity, std::uint32_t opDesc, std::uint32_t first, std::uint32_t primitives) {
    return tableMix(tableMix(tableMix(reinterpret_cast<std::uintptr_t>(entity), opDesc), first), primitives);
}

void RtScene::setTableEntries(bool on, bool compare) {
    {  // diagnostics: keep running through DIFFERING (the counts show how often)
        char buf[8];
        tableNoTrip_ = GetEnvironmentVariableA("RVK_TABLE_NOTRIP", buf, sizeof(buf)) != 0;
    }
    tableCompare_ = compare;
    if (on == tableOn_) return;
    tableOn_ = on && !tableTripped_;
    if (!tableOn_) tableClear();
    spdlog::info("rt: scene table: persistent entries {}{}", tableOn_ ? "on" : "off", tableOn_ && compare ? std::format(" (check frame every {} frames)", kTableCompare) : "");
}

void RtScene::tableClear() {
    for (auto& [k, e] : table_) {
        if (rt::Registry::Instance* in = registry_.find(e.instKey)) in->pinned = false;
        ++entryGen_[e.idx];
        entryFree_.push_back(e.idx);
    }
    table_.clear();
    tableByInst_.clear();
    tableByOp_.clear();
}

void RtScene::tableErase(std::unordered_map<std::uint64_t, TableEntry, IdHash>::iterator it, bool unpin) {
    const TableEntry& e = it->second;
    if (unpin) {
        if (rt::Registry::Instance* in = registry_.find(e.instKey)) in->pinned = false;
    }
    bool others = false;
    {
        const auto range = tableByInst_.equal_range(e.instKey);
        for (auto o = range.first; o != range.second;) {
            if (o->second == it->first) {
                o = tableByInst_.erase(o);
            } else {
                others = true;
                ++o;
            }
        }
    }
    if (unpin && others) {
        if (rt::Registry::Instance* in = registry_.find(e.instKey)) in->pinned = true;  // (another entry still holds it)
    }
    const std::uint64_t opKey = tableMix(reinterpret_cast<std::uintptr_t>(e.entity), e.opDesc);
    const auto range = tableByOp_.equal_range(opKey);
    for (auto o = range.first; o != range.second; ++o) {
        if (o->second == it->first) {
            tableByOp_.erase(o);
            break;
        }
    }
    ++entryGen_[e.idx];  // (the covers that point at it are dead)
    entryFree_.push_back(e.idx);
    table_.erase(it);
}

// F2c-4b: an instance several unlike draws gave different materials: its entries go and it is not held again (its draws are observed, as without the table); it ages as an observed one
void RtScene::tableDropInstance(std::uint64_t instKey) {
    std::vector<std::uint64_t> keys;
    const auto range = tableByInst_.equal_range(instKey);
    for (auto o = range.first; o != range.second; ++o) keys.push_back(o->second);
    if (keys.empty()) return;
    for (const std::uint64_t k : keys) {
        if (const auto it = table_.find(k); it != table_.end()) {
            tableErase(it, false);
            ++ts_.ambiguousDropped;
        }
    }
    tableByInst_.erase(instKey);
    if (rt::Registry::Instance* in = registry_.find(instKey)) in->pinned = false;
}

// the registry dropped a pinned instance (aged by distance, its BLAS died, replaced, capped): its entry goes with it
void RtScene::tableInstanceRemoved(std::uint64_t instKey) {
    std::vector<std::uint64_t> keys;
    const auto range = tableByInst_.equal_range(instKey);
    for (auto o = range.first; o != range.second; ++o) keys.push_back(o->second);
    if (keys.empty()) return;
    ++ts_.instGone;
    for (const std::uint64_t k : keys) {
        if (const auto it = table_.find(k); it != table_.end()) tableErase(it, false);
    }
    tableByInst_.erase(instKey);
}

bool RtScene::coversDraw(const d3d9::CoverQuery& d) {
    if (!tableOn_ || !active_ || d.kind != Kind::StaticMesh || d.instanced || d.blending || d.primitiveType != 4 || d.tag == nullptr ||
        d.tag->entity == nullptr || d.vertexBuffer == nullptr || d.indexBuffer == nullptr) {
        return false;
    }
    const std::uint8_t cls = tableClass(d.phase);
    if (cls == 0) return false;
    const bool timed = dev_.perfEnabled();
    const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const int ci = cls == 1 ? 0 : cls == 2 ? 1 : 2;
    ++ts_.asked[ci];
    bool covered = false;
    do {
        const auto it = table_.find(tableKey(d.tag->entity, d.tag->opDesc, d.first, d.primitives));
        if (it == table_.end()) {
            ++ts_.fallNone;
            break;
        }
        const TableEntry& e = it->second;
        if (e.entity != d.tag->entity || e.opDesc != d.tag->opDesc) {
            ++ts_.fallIdent;
            break;
        }
        if ((e.classMask & cls) == 0) {
            ++ts_.fallClass;
            break;
        }
        if (engine::scenetable::stamp(e.slot) != e.slotStamp) {  // (freed / reallocated / moved: the compact stamp of the slot)
            ++ts_.fallMoved;
            break;
        }
        // the BLAS key of the draw: the same buffers' generations, offsets and range as when it was learned
        const d3d9::ResourceRef vr = d.vertexBuffer->ref(), ir = d.indexBuffer->ref();
        const rt::BlasKey& k = e.key;
        if (k.vb != vr.id || k.vbGen != vr.generation || k.ib != ir.id || k.ibGen != ir.generation || k.vertexOffset != d.vertexOffset ||
            k.stride != d.vertexStride || k.first != d.first || k.primitives != d.primitives || k.baseVertex != d.baseVertex) {
            ++ts_.fallIdent;
            break;
        }
        if (RtMaterials::signature(d.engineVs, d.enginePs, d.tag->material, d.textures) != e.matSig[ci]) {
            ++ts_.fallMaterial;  // another shader / texture (a streamed-in image): the material is resolved again by the full observation
            break;
        }
        const std::uint64_t frame = dev_.frameValue();
        if (tableCompareFrame(frame)) {
            tableCheckFrame_ = frame;
            ++ts_.fallCompare;
            break;
        }
        if ((frame + e.salt) % kTableRelearn == 0) {
            ++ts_.fallRelearn;
            break;
        }
        ++ts_.covered[ci];
        covered = true;
        coveredEntry_ = &e;
    } while (false);
    if (timed) ts_.coverMs += msSince(t0);
    return covered;
}

// F2c-4b / F8-2 (frame-ownership.md 7.23): the persistent owned static draws of a pass. A record whose last sighting was covered by an entry skips everything while nothing it
// depends on moved: the retained draw (its own pointer), its derivation signature (texture slots / ready state, buffer bindings: the BLAS key's generations and the material's
// inputs), the pass's env epoch, the entry (alive: tableErase / tableClear bump entryGen_), the entity's scene-table slot (generation / version: the rows) and the relearn
// period; check frames always take the long way. Everything else is the per-draw procedure of ownedObserve: the pre-filter, then the full observation.
void RtScene::observeOwned(d3d9::Device& dev, const d3d9::OwnedFrame& f) {
    if (!active_) return;
    ++bs_.calls;
    bs_.left += f.leftCount;
    const std::uint64_t frame = dev.frameValue();
    const bool compare = tableCompareFrame(frame);
    for (std::size_t i = 0; i < f.count; ++i) {
        const d3d9::Device::OwnedRecordView& r = f.records[i];
        if ((r.wanted & f.observerBit) == 0) continue;
        ++bs_.records;
        bs_.entered += r.entered ? 1 : 0;
        const d3d9::Device::CachedDraw& cd = *r.cd;
        if (r.id >= ownedCover_.size()) ownedCover_.resize(static_cast<std::size_t>(r.id) + 256);
        OwnedCover& oc = ownedCover_[r.id];
        if (!r.entered && !r.changed && !compare && oc.entry != nullptr && oc.cd == &cd && oc.sig == r.sig && oc.epoch == r.epoch && oc.entity == r.tag.entity &&
            oc.opDesc == r.tag.opDesc && oc.idx < entryGen_.size() && entryGen_[oc.idx] == oc.gen) {
            if (engine::scenetable::stamp(oc.slot) == oc.slotStamp && (frame + oc.salt) % kTableRelearn != 0) {
                const int ci = oc.cls == 1 ? 0 : oc.cls == 2 ? 1 : 2;
                ++ts_.asked[ci];
                ++ts_.covered[ci];
                ++bs_.skipped;
                continue;
            }
        }
        ++bs_.slow;
        oc.entry = nullptr;
        d3d9::CoverQuery cq = d3d9::coverOf(cd.query);
        cq.tag = &r.tag;
        cq.textures = cd.tex;
        coveredEntry_ = nullptr;
        if (coversDraw(cq) && coveredEntry_ != nullptr) {
            oc.cd = &cd;
            oc.entity = r.tag.entity;
            oc.opDesc = r.tag.opDesc;
            oc.sig = r.sig;
            oc.epoch = r.epoch;
            oc.entry = coveredEntry_;
            oc.idx = coveredEntry_->idx;
            oc.slot = coveredEntry_->slot;
            oc.slotStamp = coveredEntry_->slotStamp;
            oc.salt = coveredEntry_->salt;
            oc.gen = entryGen_[oc.idx];
            oc.cls = tableClass(cd.phase);
            ++bs_.covered;
            continue;
        }
        d3d9::Device::OwnedDrawScope scope(dev, r);
        scope.loadRegisters();
        observe(dev, scope.query());
        ++bs_.observed;
    }
}

// a plain draw of the table's class has just been observed in full (`in` = its registry instance after the sighting): compare it with its entry
// (check frames), then learn / refresh the entry and pin the instance
void RtScene::tableObserved(const d3d9::ReplaceQuery& q, const Blas& b, rt::Registry::Instance& in, const rt::Rows& rows, bool fresh,
                            const rt::Registry::Instance* pre) {
    if (q.kind != Kind::StaticMesh || q.tag == nullptr || q.tag->entity == nullptr || q.primitiveType != 4 || q.blending || q.instanced || b.glass ||
        b.animatedOnly || q.tag->water != d3d9::draws::Water::None) {
        return;
    }
    const std::uint8_t cls = tableClass(q.phase);
    if (cls == 0) return;
    const int ci = cls == 1 ? 0 : cls == 2 ? 1 : 2;
    ++ts_.observed[ci];
    const std::uint64_t frame = dev_.frameValue();
    const std::uint64_t ek = tableKey(q.tag->entity, q.tag->opDesc, q.first, q.primitives);
    auto it = table_.find(ek);
    if (in.ambiguous) {  // F2c-4b: unlike draws disagree on this instance's material: it stays on the observed path
        ++ts_.refusedAmbiguous;
        if (it != table_.end()) tableErase(it, true);
        return;
    }
    const std::uint32_t slotIndex = engine::scenetable::find(q.tag->entity);
    const engine::scenetable::Slot* slot = engine::scenetable::slot(slotIndex);
    if (slot == nullptr) {
        ++ts_.refusedNoSlot;
        if (it != table_.end()) tableErase(it, true);
        return;
    }
    rt::Rows derived;
    rowsOfWorld(slot->world, derived);
    if (!rowsNear(derived, rows)) {
        ++ts_.refusedRows;  // a hierarchy node / carried matrix: not the entity's own world matrix
        if (it != table_.end()) tableErase(it, true);
        return;
    }
    // the check frame: the entry as the last full observation left it against what this one found
    if (tableCompareFrame(frame)) {
        if (it != table_.end() && (it->second.classMask & cls) != 0 && it->second.slotGen == slot->generation && it->second.slotVersion == slot->version) {
            TableEntry& e = it->second;
            e.comparedFrame = frame;
            bool bad = false;
            if (pre == nullptr || e.instKey != in.key || e.blas != b.id || !(e.key == b.key)) {
                ++ts_.diffKey;
                bad = true;
            }
            if (!rowsNear(e.rows, rows) || !rowsNear(derived, e.rows)) {
                ++ts_.diffRows;
                bad = true;
            }
            // what the covered draw would have skipped: the sighting must change nothing of the instance it had (flags, data word, view bit, material)
            if (pre != nullptr) {
                if (pre->flags != in.flags) {
                    ++ts_.diffFlags;
                    bad = true;
                }
                if ((pre->data & 0xFFFFu) != (in.data & 0xFFFFu)) {  // (the kind in the high bits is the latest sighting's, diagnostics only)
                    ++ts_.diffData;
                    bad = true;
                }
                if ((pre->views & viewBit(q.phase.view)) != viewBit(q.phase.view)) {
                    ++ts_.diffFlags;
                    bad = true;
                }
                // the material is judged on the frame's final state (several draws over one instance: the last one counts): the first pre-state of each instance
                tableMatPre_.emplace(in.key, pre->material);
            }
            bad ? ++ts_.differing : ++ts_.matched;
            if (bad && ts_.differing <= 8) {
                spdlog::warn("rt: scene table: DIFFERING entry (entity {} op {} range {} x {}, class {}, view {}): key {} rows {} flags 0x{:x} -> 0x{:x} views {} -> {} data 0x{:x} -> 0x{:x} material {} -> {}",
                             q.tag->entity, q.tag->opDesc, q.first, q.primitives, cls, static_cast<int>(viewBit(q.phase.view)), pre != nullptr && e.instKey == in.key ? "same" : "differs",
                             rowsNear(e.rows, rows) ? "same" : "differ", pre != nullptr ? pre->flags : 0u, in.flags, pre != nullptr ? pre->views : 0u, in.views,
                             pre != nullptr ? pre->data : 0u, in.data, pre != nullptr ? pre->material : 0u, in.material);
            }
        } else if (it == table_.end() && !fresh && in.firstFrame + 2 <= frame) {
            ++ts_.missing;  // an old instance of an eligible draw that no entry held
        }
    }
    // learn / refresh
    if (it != table_.end()) {
        TableEntry& e = it->second;
        const bool same = e.instKey == in.key && e.blas == b.id && e.key == b.key && e.slotGen == slot->generation && e.slotVersion == slot->version;
        if (!same) {
            tableErase(it, true);
            it = table_.end();
        } else {
            e.classMask |= cls;
            e.matSig[ci] = RtMaterials::signature(q);
            e.flags = in.flags;
            e.data = in.data;
            e.material = in.material;
            in.pinned = true;
            ++ts_.relearned;
            return;
        }
    }
    // a level-of-detail switch: the entity's same op drawn from another vertex / index buffer replaces the entries of the old one
    const std::uint64_t opKey = tableMix(reinterpret_cast<std::uintptr_t>(q.tag->entity), q.tag->opDesc);
    {
        std::vector<std::uint64_t> stale;
        const auto range = tableByOp_.equal_range(opKey);
        for (auto o = range.first; o != range.second; ++o) {
            const auto other = table_.find(o->second);
            if (other == table_.end()) continue;
            if (other->second.key.vb != b.key.vb || other->second.key.vbGen != b.key.vbGen || other->second.key.ib != b.key.ib ||
                other->second.key.ibGen != b.key.ibGen) {
                stale.push_back(o->second);
            }
        }
        for (const std::uint64_t k : stale) {
            if (const auto o = table_.find(k); o != table_.end()) {
                tableErase(o, true);
                ++ts_.lodErased;
            }
        }
    }
    TableEntry e;
    if (!entryFree_.empty()) {
        e.idx = entryFree_.back();
        entryFree_.pop_back();
    } else {
        e.idx = static_cast<std::uint32_t>(entryGen_.size());
        entryGen_.push_back(1);
    }
    e.entity = q.tag->entity;
    e.opDesc = q.tag->opDesc;
    e.first = q.first;
    e.primitives = q.primitives;
    e.slot = slotIndex;
    e.slotGen = slot->generation;
    e.slotVersion = slot->version;
    e.slotStamp = engine::scenetable::stamp(slotIndex);
    e.key = b.key;
    e.blas = b.id;
    e.instKey = in.key;
    e.rows = rows;
    e.flags = in.flags;
    e.data = in.data;
    e.material = in.material;
    e.classMask = cls;
    e.salt = static_cast<std::uint8_t>(tableMix(in.key, 0x5A17) >> 40);  // per instance: every entry over one instance is observed again in the same frame (its final material is the last draw's)
    e.matSig[ci] = RtMaterials::signature(q);
    e.comparedFrame = tableCompareFrame(frame) ? frame : 0;
    table_.emplace(ek, e);
    tableByInst_.emplace(in.key, ek);
    tableByOp_.emplace(opKey, ek);
    in.pinned = true;
    ++ts_.learned;
}

// once per frame (the RT scene's pass, before the registry ages): an entry whose scene-table slot was freed, reallocated or moved no longer pins its
// instance (the instance ages as before; a moved object's draws fall through to the full observation and make a new one)
void RtScene::tableMaintain(std::uint64_t frame) {
    if (!tableOn_) return;
    if (!tableMatPre_.empty() && frame > tableCheckFrame_) {
        // the previous check frame's materials: the instance's final material against its first pre-state (same content, or the old one died)
        for (const auto& [key, preMaterial] : tableMatPre_) {
            const rt::Registry::Instance* in = registry_.find(key);
            if (in == nullptr || in->material == preMaterial) continue;
            if (in->ambiguous) {  // F2c-4b: unlike draws disagree (the instance is no longer held by an entry): the difference is the explained one
                ++ts_.ambiguousSkipped;
                continue;
            }
            const RtMaterial* a = preMaterial != 0 ? materials_.get(preMaterial) : nullptr;
            const RtMaterial* c = in->material != 0 ? materials_.get(in->material) : nullptr;
            const bool same = a != nullptr && c != nullptr && a->albedo == c->albedo && a->opacity == c->opacity && a->sampler == c->sampler &&
                              a->uvSet == c->uvSet && a->alpha == c->alpha && a->masked == c->masked && a->colour == c->colour;
            if (a == nullptr || a->dead || same) {
                ++ts_.materialIds;
            } else {
                ++ts_.diffMaterial;
                ++ts_.differing;
                if (ts_.diffMaterial <= 4) {
                    spdlog::warn("rt: scene table: DIFFERING material of instance {}: {} '{}' -> {} '{}' over a check frame (albedo {}/{} opacity {}/{} sampler {}/{} uvSet 0x{:x}/0x{:x} alpha {}/{} masked {}/{} colour {:x}/{:x} dead {}/{})", key, preMaterial,
                                 a != nullptr ? a->name : std::string("gone"), in->material, c != nullptr ? c->name : std::string("none"),
                                 a ? static_cast<const void*>(a->albedo) : nullptr, c ? static_cast<const void*>(c->albedo) : nullptr, a ? static_cast<const void*>(a->opacity) : nullptr, c ? static_cast<const void*>(c->opacity) : nullptr,
                                 a ? a->sampler : 0u, c ? c->sampler : 0u, a ? a->uvSet : 0u, c ? c->uvSet : 0u, a ? a->alpha : 0u, c ? c->alpha : 0u, a ? a->masked : false, c ? c->masked : false,
                                 a ? a->colour : 0u, c ? c->colour : 0u, a ? a->dead : false, c ? c->dead : false);
                }
            }
        }
        tableMatPre_.clear();
    }
    if (tableCheckFrame_ != 0 && tableCheckFrame_ != tableExtraCounted_ && frame > tableCheckFrame_) {
        // the previous check frame: entries none of whose draws was observed on it (off-screen / not drawn now) stay
        for (const auto& [k, e] : table_) ts_.extra += e.comparedFrame != tableCheckFrame_ ? 1u : 0u;
        ++ts_.checkFrames;
        tableExtraCounted_ = tableCheckFrame_;
    }
    for (auto it = table_.begin(); it != table_.end();) {
        const TableEntry& e = it->second;
        if (engine::scenetable::stamp(e.slot) != e.slotStamp) {
            const auto victim = it++;
            tableErase(victim, true);
            ++ts_.slotDropped;
        } else {
            ++it;
        }
    }
}

void RtScene::logTable(double frames) {
    if (!tableOn_ && ts_.asked[0] + ts_.asked[1] + ts_.asked[2] + ts_.learned == 0) return;
    std::size_t pinned = 0, mainOnly = 0, shadowOnly = 0, both = 0;
    for (const rt::Registry::Instance& in : registry_.instances()) {
        pinned += in.pinned ? 1 : 0;
        const bool m = (in.views & 1) != 0, sh = (in.views & 2) != 0;
        (m && sh ? both : m ? mainOnly : shadowOnly) += 1;
    }
    const TableStats& t = ts_;
    const auto sum3 = [](const std::uint64_t (&a)[3]) { return a[0] + a[1] + a[2]; };
    const std::uint64_t asked = sum3(t.asked), covered = sum3(t.covered);
    spdlog::info("rt: scene table (5 s): entries {} (pinned instances {} of {}: views main-only {} / cascade-only {} / both {}), per frame: covered draws skipped {:.0f} of {:.0f} asked ({:.1f} %; "
                 "main solid {}/{}, z {}/{}, cascades {}/{}), full observations of the class {:.0f}; fall-throughs: no entry {}, class {}, moved {}, buffers / range {}, material inputs {}, relearn {}, check frame {}; "
                 "learned {} (refreshed {}), refused: no slot {} / rows {}; erased: LOD {} / slot {} / instance gone {}; compare: matched {} / missing {} / extra {:.0f} per check frame ({} check frames) / DIFFERING {} "
                 "(key {} rows {} flags {} data {} material {}; material ids changed {}); observe {:.3f} ms/frame, pre-filter {:.3f} ms/frame",
                 table_.size(), pinned, registry_.size(), mainOnly, shadowOnly, both, static_cast<double>(covered) / frames, static_cast<double>(asked) / frames,
                 asked != 0 ? 100.0 * static_cast<double>(covered) / static_cast<double>(asked) : 0.0, t.covered[0], t.asked[0], t.covered[1], t.asked[1], t.covered[2], t.asked[2],
                 static_cast<double>(sum3(t.observed)) / frames, t.fallNone, t.fallClass, t.fallMoved, t.fallIdent, t.fallMaterial, t.fallRelearn, t.fallCompare, t.learned, t.relearned,
                 t.refusedNoSlot, t.refusedRows, t.lodErased, t.slotDropped, t.instGone, t.matched, t.missing, static_cast<double>(t.extra) / static_cast<double>(std::max<std::uint64_t>(1, t.checkFrames)), t.checkFrames, t.differing, t.diffKey, t.diffRows, t.diffFlags,
                 t.diffData, t.diffMaterial, t.materialIds, iv_.observeMs / frames, t.coverMs / frames);
    // key / rows / flags / data differences are never expected; a material that differs from the frame's final one is a rare ordering case of several draws
    // over one instance (a few per million compared draws): only a rate above 0.5 % trips
    const std::uint64_t hard = t.diffKey + t.diffRows + t.diffFlags + t.diffData;
    const bool tripped = hard != 0 || t.diffMaterial > std::max<std::uint64_t>(8, (t.matched + t.diffMaterial) / 200);
    if (tripped && tableOn_ && !tableNoTrip_) {
        tableTripped_ = true;
        tableOn_ = false;
        tableClear();
        spdlog::error("rt: scene table: turned off: {} entries DIFFERING from the full observation on the check frames (rt.table_entries stays set; restart to retry)", t.differing);
    }
    if (bs_.calls != 0 || t.refusedAmbiguous != 0 || t.ambiguousDropped != 0) {
        spdlog::info("rt: scene table bulk (5 s): {} observeOwned calls, per frame {:.0f} records ({:.1f} entered, {:.1f} left): skipped without a lookup {:.0f}, long way {:.0f} (covered by the pre-filter {:.0f}, observed in full {:.0f}); "
                     "ambiguous instances (unlike draws, different materials): learns refused {}, entries dropped {}, material differences skipped on check frames {}",
                     bs_.calls, static_cast<double>(bs_.records) / frames, static_cast<double>(bs_.entered) / frames, static_cast<double>(bs_.left) / frames, static_cast<double>(bs_.skipped) / frames,
                     static_cast<double>(bs_.slow) / frames, static_cast<double>(bs_.covered) / frames, static_cast<double>(bs_.observed) / frames, t.refusedAmbiguous, t.ambiguousDropped, t.ambiguousSkipped);
    }
    bs_ = {};
    ts_ = {};
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
    // F2c-4b: a second opaque draw of the frame that resolves another material: the winner is the last observed draw, which depends on which draws are observed
    if (rank == 4 && in.sightFrame == frame && in.sightRank == 4 && in.material != 0 && id != 0 && in.material != id) in.ambiguous = true;
    // blended layers: the first draw of the frame at this rank counts (the base); opaque passes over the same surface: the
    // last one (it covers the earlier ones in the depth buffer)
    if (rank != 4 && in.sightFrame == frame && in.sightRank == rank) return;
    in.sightFrame = frame;
    in.sightRank = rank;
    in.material = id;
    in.materialRank = rank;
}

RtScene::MaterialFields RtScene::materialFields(const RtMaterial* m) const {
    MaterialFields f;
    const auto slotOf = [](d3d9::Texture* t) -> std::uint32_t {
        return t != nullptr && !t->isNull() && t->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? t->bindlessSlot() : 0u;
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
    if (pack_ != nullptr && !m->name.empty()) {
        // (the map must be uploaded: the raster draws and the pack's prepare() do that; until then the entry is not used)
        const auto hit = pack_->lookup(m->name);
        if (hit.word != 0 && (hit.texture == nullptr || hit.texture->layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) f.packWord = hit.word;
        else if (hit.word == 0) f.packWord = pack_->deriveWord(m->spec);  // no entry: the roughness the engine's specular data implies (materials.derive_from_specular)
    }
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
    // (the RHI objects are destroyed with the frame being recorded: the GPU may still use them)
    if (b->as != nullptr) {
        blasBytes_ -= std::min(blasBytes_, b->as->storageBytes());
        b->as.reset();
    }
    dropProxy(*b);
    if (b->released && released_ > 0) --released_;
    if (b->listIndices != nullptr) {
        blasBytes_ -= std::min(blasBytes_, b->listIndices->desc().size);
        b->listIndices.reset();
    }
    if (countEvicted) ++iv_.evicted;
    blas_.erase(id);
}

// ---- GPU resources --------------------------------------------------------------------------------------------------------

bool RtScene::ensureResources() {
    if (tlas_[0].as != nullptr) return true;
    rhi::Device* rd = dev_.rhi();
    bool ok = rd != nullptr && rd->caps().rayQuery;
    const std::uint32_t cap = limits_.maxInstances;
    // the TLAS of each frame slot, sized for the cap (the HLSL ray queries reach it through the bindless acceleration structure array; its slot is the
    // RHI object's, allocated from the renderer's set)
    std::uint64_t tlasBytes = 0;
    for (Tlas& t : tlas_) {
        if (!ok) break;
        rhi::TlasDesc td;
        td.maxInstances = cap;
        td.preferFastBuild = true;  // rebuilt every frame
        td.name = "rt scene tlas";
        t.as = rd->createTlas(td);
        ok = t.as != nullptr;
        t.capacity = cap;
        t.slot = ok ? t.as->tlasSlot() : 0;
        tlasBytes = ok ? t.as->storageBytes() : 0;
    }
    // compaction-size queries of each slot's builds
    if (ok) {
        compactQuery_ = rd->createSizeQuery(kQueriesPerSlot * rhi::kFramesInFlight);
        ok = compactQuery_ != nullptr;
    }
    for (auto& c : compactions_) c.clear();
    if (!ok) {
        spdlog::error("rt: scene: GPU resources failed, the RT scene stays off this session");
        failed_ = true;
        destroyResources();
        return false;
    }
    spdlog::info("rt: scene: TLAS {} instances = {:.1f} MiB x {}, BLAS build scratch from the RHI's shared pool (grows on demand)", cap,
                 static_cast<double>(tlasBytes) / 1048576.0, rhi::kFramesInFlight);
    return true;
}

// (the RHI objects are destroyed with the frame being recorded: the GPU may still use them; a TLAS gives its slot back)
void RtScene::destroyResources() {
    for (Tlas& t : tlas_) t = {};
    compactQuery_.reset();
    for (auto& c : compactions_) c.clear();
    box_ = {};
}

void RtScene::barrier(rhi::CommandList& cl, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess) {
    cl.memoryBarrier(srcStage, srcAccess, dstStage, dstAccess);
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

// RVK_NO_COMPACT (environment): no BLAS compaction (diagnostics)
static bool noCompaction() {
    static const bool v = GetEnvironmentVariableA("RVK_NO_COMPACT", nullptr, 0) != 0;
    return v;
}

void RtScene::processCompactions(rhi::CommandList& cl, std::uint32_t slot) {
    auto& list = compactions_[slot];
    if (list.empty()) return;
    rhi::Device* rd = dev_.rhi();
    std::vector<std::uint64_t> sizes(list.size(), 0);
    // (an entry that is not available reads 0: the slot's earlier submission completed, so none should be missing)
    compactQuery_->read(slot * kQueriesPerSlot, static_cast<std::uint32_t>(list.size()), sizes.data());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Compaction& c = list[i];
        if (sizes[i] == 0) continue;
        Blas* b = findBlas(c.blas);
        if (b == nullptr || b->serial != c.serial || b->state != BlasState::Built || b->compacted || b->as == nullptr) continue;
        b->compacted = true;
        const std::uint64_t size = sizes[i];
        if (static_cast<double>(size) > 0.9 * static_cast<double>(b->as->storageBytes())) continue;
        std::unique_ptr<rhi::AccelerationStructure> dst = rd->createBlasStorage(size, "rt scene blas (compacted)");
        if (dst == nullptr) continue;
        cl.compactBlas(*dst, *b->as);
        // the old structure goes with the frame being recorded (the copy reads it)
        const std::uint64_t oldBytes = b->as->storageBytes();
        iv_.compactSaved += oldBytes - dst->storageBytes();
        blasBytes_ = blasBytes_ - std::min(blasBytes_, oldBytes) + dst->storageBytes();
        b->as = std::move(dst);
        ++iv_.compacted;
    }
    list.clear();
}

void RtScene::buildPending(rhi::CommandList& cl, std::uint32_t slot) {
    if (pending_.empty()) return;
    rhi::Device* rd = dev_.rhi();
    rt::BuildBudget budget(limits_.buildsPerFrame, limits_.trianglesPerFrame);
    const std::size_t maxBuilds = std::min<std::size_t>(limits_.buildsPerFrame, kQueriesPerSlot);
    // (the builds hold spans into `geoms`: no reallocation)
    std::vector<rhi::TriangleGeometry> geoms;
    std::vector<rhi::BlasBuild> builds;
    geoms.reserve(maxBuilds);
    builds.reserve(maxBuilds);
    std::vector<std::uint64_t> built;
    bool copied = false;  // strip conversions copied into their index buffers: visible to the builds (and the shaders) first
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
        const auto failBuild = [&] {
            b->state = BlasState::Failed;
            b->rebuild = false;
            if (b->released && released_ > 0) --released_;
            b->released = false;
            ++iv_.failed;
            registry_.removeBlas(b->id);
        };
        if (!prepareInputs(*b)) {
            failBuild();
            continue;
        }
        const std::uint32_t isize = k.index32 != 0 ? 4u : 2u;
        if (k.strip != 0) {
            // the converted list into a device buffer of its own (through the frame's ring)
            const std::uint64_t bytes = b->listData.size();
            const rhi::RingAlloc up = dev_.ring(bytes, 16);
            if (up.ptr != nullptr) {
                b->listIndices = rd->createBuffer({bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device,
                                                   "rt scene strip indices"});
            }
            if (up.ptr == nullptr || b->listIndices == nullptr) {
                failBuild();
                continue;
            }
            std::memcpy(up.ptr, b->listData.data(), bytes);
            cl.copyBuffer(*b->listIndices, 0, dev_.game().ringBuffer(up), up.offset, bytes);
            b->listData.clear();
            b->listData.shrink_to_fit();
            blasBytes_ += bytes;
            copied = true;
        }
        rhi::TriangleGeometry g;
        g.vertexAddress = b->vbAddress.plus(static_cast<std::int64_t>(k.vertexOffset) + static_cast<std::int64_t>(k.baseVertex) * k.stride).va;
        g.vertexStride = k.stride;
        g.vertexCount = b->maxVertex + 1;
        g.indexType = k.index32 != 0 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        g.indexAddress = k.strip != 0 ? b->listIndices->gpuAddress() : b->ibAddress.plus(static_cast<std::int64_t>(k.first) * isize).va;
        g.indexCount = b->triangles * 3;
        g.opaque = true;  // alpha-tested instances: FORCE_NO_OPAQUE on the instance
        rhi::BlasDesc desc;
        desc.geometries = std::span<const rhi::TriangleGeometry>(&g, 1);
        desc.allowCompaction = !noCompaction();  // (not allowUpdate: a BLAS that is both did not compact into a traceable structure on the AMD driver, hlsl-rhi.md 6c)
        desc.name = "rt scene blas";
        std::unique_ptr<rhi::AccelerationStructure> as = rd->createBlas(desc);
        if (as == nullptr) {
            if (reasonsLogged_.insert(kReasonAsCreate).second) spdlog::warn("rt: scene: a BLAS of {} triangles could not be created", b->triangles);
            failBuild();
            continue;
        }
        b->as = std::move(as);
        if (b->released) {
            b->released = false;
            if (released_ > 0) --released_;
        }
        b->rebuild = false;
        geoms.push_back(g);
        builds.push_back({b->as.get(), std::span<const rhi::TriangleGeometry>(&geoms.back(), 1), false});
        b->state = BlasState::Built;
        ++b->serial;
        blasBytes_ += b->as->storageBytes();
        built.push_back(b->id);
        ++iv_.built;
        iv_.builtTriangles += b->triangles;
        iv_.buildCpuMs += msSince(c0);
    }
    if (builds.empty()) return;
    if (copied) {
        cl.memoryBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    cl.buildBlases(builds);
    // the builds -> their compacted-size queries (and the TLAS build after them)
    std::vector<rhi::AccelerationStructure*> handles;
    auto& list = compactions_[slot];
    list.clear();
    for (std::uint64_t id : built) {
        const Blas* b = findBlas(id);
        if (b == nullptr) continue;
        list.push_back({id, b->serial, static_cast<std::uint32_t>(handles.size())});
        handles.push_back(b->as.get());
    }
    if (handles.empty() || noCompaction()) {
        list.clear();
        return;
    }
    cl.queryCompactedSizes(*compactQuery_, slot * kQueriesPerSlot, handles);
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
// ---- level of detail (docs/research/rt-lod.md) -----------------------------------------------------------------------------

void RtScene::setLod(float rangeMetres, int bias, float farRangeMetres, float scale) {
    const int b = std::clamp(bias, 0, 3);
    const bool wasOn = lodActive();
    if (b != lodBias_) {
        // another level: every proxy is made again
        for (auto& [id, blas] : blas_) dropProxy(blas);
        proxyJobs_.clear();
    }
    lodBias_ = b;
    lodBaseRange_ = std::max(0.0f, rangeMetres) * 100.0f;
    lodBaseFar_ = std::max(0.0f, farRangeMetres) * 100.0f;
    lodScale_ = std::max(0.0f, scale);
    applyLodEffective();
    if (wasOn != lodActive() || lodActive()) {
        spdlog::info("rt: scene: lod {} (proxy beyond {:.0f} m, bias {}, box beyond {:.0f} m; base {:.0f} / {:.0f} m x scale {:.2f}, budget scale {:.2f})",
                     lodActive() ? "on" : "off", lodRangeUnits_ / 100.0f, b, lodFarUnits_ / 100.0f, rangeMetres, farRangeMetres, scale, budgetScale_);
    }
}

void RtScene::setMemoryBudget(std::uint32_t megabytes) {
    budgetBytes_ = static_cast<std::uint64_t>(megabytes) << 20;
    budgetScale_ = 1.0f;
    limits_.memoryBudget = budgetBytes_ != 0 ? std::min<std::uint64_t>(budgetBytes_, 512ull << 20) : (512ull << 20);
    applyLodEffective();
    spdlog::info("rt: scene: memory limit {}", megabytes != 0 ? std::format("{} MB", megabytes) : std::string("none"));
}

// the effective ranges: base x scale x the budget's scale; scale 0 = off unless a memory limit asks for the ladder (base ranges then)
void RtScene::applyLodEffective() {
    const float s = (lodScale_ > 0.0f ? lodScale_ : (budgetBytes_ != 0 ? 1.0f : 0.0f)) * budgetScale_;
    lodRangeUnits_ = lodBias_ > 0 && s > 0.0f ? lodBaseRange_ * s : 0.0f;
    lodFarUnits_ = s > 0.0f ? lodBaseFar_ * s : 0.0f;
    if (!lodActive()) {
        for (rt::Registry::Instance& in : registry_.instancesMutable()) in.lod = 0;
    }
}

std::uint64_t RtScene::asBytes() const {
    const std::uint64_t tlas = tlas_[0].as != nullptr ? tlas_[0].as->storageBytes() * rhi::kFramesInFlight : 0;
    return blasBytes_ + tlas;
}

// gives up the original AS of BLASes that no instance of this frame traces at full detail (a proxy or the box stands in), largest first,
// until the AS memory is at `target`
void RtScene::releaseOriginals(std::uint64_t target) {
    if (!box_.built) return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> cand;  // bytes, id
    for (const auto& [id, b] : blas_) {
        if (b.released || b.state != BlasState::Built || b.as == nullptr || nearBlas_.contains(id) || b.animatedOnly || !b.bounds.valid()) continue;
        cand.emplace_back(b.as->storageBytes(), id);
    }
    std::sort(cand.begin(), cand.end(), std::greater<>());
    for (const auto& [bytes, id] : cand) {
        if (asBytes() <= target) break;
        Blas* b = findBlas(id);
        if (b == nullptr) continue;
        blasBytes_ -= std::min(blasBytes_, b->as->storageBytes());
        b->as.reset();  // (destroyed with the frame being recorded: the GPU may still use it)
        b->released = true;
        ++released_;
        ++releasedTotal_;
    }
}

void RtScene::requestRebuild(Blas& b) {
    if (!b.released || b.rebuild) return;
    b.rebuild = true;
    b.state = BlasState::Pending;
    pending_.push_back(b.id);
    ++rebuiltTotal_;
}

// The shared box of rung 2: a unit cube [-1, 1]^3 (8 vertices, 36 u16 indices) and its BLAS, built once.
bool RtScene::ensureBox(rhi::CommandList& cl) {
    if (box_.built) return true;
    if (box_.failed) return false;
    rhi::Device* rd = dev_.rhi();
    static constexpr std::uint16_t kIdx[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    std::uint8_t data[96 + 72];
    float verts[24];
    for (int i = 0; i < 8; ++i) {
        verts[i * 3] = (i & 1) != 0 ? 1.0f : -1.0f;
        verts[i * 3 + 1] = (i & 2) != 0 ? 1.0f : -1.0f;
        verts[i * 3 + 2] = (i & 4) != 0 ? 1.0f : -1.0f;
    }
    std::memcpy(data, verts, 96);
    std::memcpy(data + 96, kIdx, 72);
    const rhi::RingAlloc up = dev_.ring(sizeof(data), 16);
    if (up.ptr == nullptr) return false;
    box_.buffer = rd->createBuffer({sizeof(data), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device, "rt scene lod box"});
    if (box_.buffer == nullptr) {
        box_.failed = true;
        return false;
    }
    std::memcpy(up.ptr, data, sizeof(data));
    cl.copyBuffer(*box_.buffer, 0, dev_.game().ringBuffer(up), up.offset, sizeof(data));
    cl.memoryBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    rhi::TriangleGeometry g;
    g.vertexAddress = box_.buffer->gpuAddress();
    g.vertexStride = 12;
    g.vertexCount = 8;
    g.indexType = VK_INDEX_TYPE_UINT16;
    g.indexAddress = box_.buffer->gpuAddress() + 96;
    g.indexCount = 36;
    g.opaque = true;
    rhi::BlasDesc desc;
    desc.geometries = std::span<const rhi::TriangleGeometry>(&g, 1);
    desc.allowCompaction = false;
    desc.name = "rt scene lod box blas";
    box_.as = rd->createBlas(desc);
    if (box_.as == nullptr) {
        box_.failed = true;
        return false;
    }
    rhi::BlasBuild build{box_.as.get(), std::span<const rhi::TriangleGeometry>(&g, 1), false};
    cl.buildBlases(std::span<const rhi::BlasBuild>(&build, 1));
    box_.built = true;
    return true;
}

// a static triangle mesh worth a proxy: a SpeedTree's cut-out parts and merged per-triangle-material ranges are not
bool RtScene::lodEligible(const Blas& b) const {
    if ((b.state != BlasState::Built && !b.rebuild) || b.animatedOnly || b.triangles < 128 || b.proxy.state == Blas::Proxy::State::Failed) return false;
    if (!b.segments.empty() || !b.engineSegments.empty()) return false;
    const auto kind = static_cast<Kind>(b.kind);
    return kind == Kind::StaticMesh || kind == Kind::InstancedMesh || kind == Kind::SpeedTree;
}

void RtScene::dropProxy(Blas& b) {
    if (b.proxy.state == Blas::Proxy::State::None) return;
    blasBytes_ -= std::min(blasBytes_, b.proxy.bytes);
    proxyBytes_ -= std::min(proxyBytes_, b.proxy.bytes);
    if (b.proxy.state == Blas::Proxy::State::Built && proxiesBuilt_ > 0) --proxiesBuilt_;
    // (the RHI objects are destroyed with the frame being recorded; a running job is waited for by the future)
    b.proxy = {};
}

// Gathers the draw's triangles + positions on this thread (the CPU copies of the buffers) and simplifies them on a worker.
void RtScene::queueProxy(Blas& b) {
    if (b.proxy.state != Blas::Proxy::State::None || proxyJobs_.size() >= 8) return;
    const std::uint64_t frame = dev_.frameValue();
    if (proxyGatherFrame_ != frame) {
        proxyGatherFrame_ = frame;
        proxyGatherTriangles_ = 0;
    }
    if (proxyGatherTriangles_ > 0 && proxyGatherTriangles_ + b.triangles > 200000) return;  // next frame
    const auto t0 = std::chrono::steady_clock::now();
    const rt::BlasKey& k = b.key;
    const std::uint32_t isize = k.index32 != 0 ? 4u : 2u;
    const std::uint32_t count = k.strip != 0 ? k.primitives + 2 : k.primitives * 3;
    const auto fail = [&] {
        b.proxy.state = Blas::Proxy::State::Failed;
        ++proxyJobsFailed_;
    };
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(count) * isize);
    if (b.vb == nullptr || b.ib == nullptr || !b.ib->readCpu(k.first * isize, count * isize, idx.data())) return fail();
    const auto at = [&](std::uint32_t i) {
        std::uint32_t v = 0;
        if (isize == 4) {
            std::memcpy(&v, idx.data() + static_cast<std::size_t>(i) * 4, 4);
        } else {
            std::uint16_t w;
            std::memcpy(&w, idx.data() + static_cast<std::size_t>(i) * 2, 2);
            v = w;
        }
        return v;
    };
    std::vector<std::uint32_t> tri;
    tri.reserve(static_cast<std::size_t>(k.primitives) * 3);
    if (k.strip != 0) {
        for (std::uint32_t t = 0; t < k.primitives; ++t) {
            const std::uint32_t a = at(t), c1 = at(t + 1), c2 = at(t + 2);
            if (a == c1 || c1 == c2 || a == c2) continue;
            tri.push_back(a);
            tri.push_back((t & 1u) != 0 ? c2 : c1);
            tri.push_back((t & 1u) != 0 ? c1 : c2);
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) tri.push_back(at(i));
    }
    if (tri.size() < 12 || k.baseVertex < 0) return fail();
    std::uint32_t lo = UINT32_MAX, hi = 0;
    for (std::uint32_t v : tri) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const std::uint64_t first = k.vertexOffset + (static_cast<std::uint64_t>(k.baseVertex) + lo) * k.stride;
    const std::uint64_t bytes = static_cast<std::uint64_t>(hi - lo) * k.stride + 12;
    if (first + bytes > b.vb->size()) return fail();
    std::vector<std::uint8_t> verts(static_cast<std::size_t>(bytes));
    if (!b.vb->readCpu(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bytes), verts.data())) return fail();
    const std::uint32_t n = hi - lo + 1;
    std::vector<float> pos(static_cast<std::size_t>(n) * 3);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::memcpy(&pos[static_cast<std::size_t>(i) * 3], verts.data() + static_cast<std::size_t>(i) * k.stride, 12);
        for (int c = 0; c < 3; ++c) {
            float& f = pos[static_cast<std::size_t>(i) * 3 + c];
            if (!std::isfinite(f)) f = 0.0f;
        }
    }
    for (std::uint32_t& v : tri) v -= lo;
    proxyGatherTriangles_ += b.triangles;
    const float ratio = rt::lod::targetRatio(lodBias_);
    b.proxy.state = Blas::Proxy::State::Queued;
    b.proxy.level = static_cast<std::uint32_t>(lodBias_);
    b.proxy.serial = b.serial;
    b.proxy.job = std::async(std::launch::async, [pos = std::move(pos), tri = std::move(tri), n, lo, ratio]() mutable {
        rt::lod::Result r = rt::lod::simplify(pos.data(), n, 12, tri, ratio, 0.03f);
        for (std::uint32_t& v : r.indices) v += lo;
        return r;
    });
    proxyJobs_.push_back(b.id);
    proxyGatherMs_ += msSince(t0);
}

// finished jobs: the index list into a device buffer of its own, a BLAS over the same vertices (no compaction: they are small)
void RtScene::buildProxies(rhi::CommandList& cl) {
    if (proxyJobs_.empty()) return;
    rhi::Device* rd = dev_.rhi();
    std::vector<rhi::TriangleGeometry> geoms;
    std::vector<rhi::BlasBuild> builds;
    constexpr std::size_t kMaxProxyBuilds = 16;
    geoms.reserve(kMaxProxyBuilds);
    builds.reserve(kMaxProxyBuilds);
    bool copied = false;
    std::vector<std::uint64_t> keep;
    for (std::uint64_t id : proxyJobs_) {
        Blas* b = findBlas(id);
        if (b == nullptr || b->proxy.state != Blas::Proxy::State::Queued || !b->proxy.job.valid()) continue;
        if (builds.size() >= kMaxProxyBuilds || b->proxy.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            keep.push_back(id);
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        rt::lod::Result res = b->proxy.job.get();
        const rt::BlasKey& k = b->key;
        const std::uint32_t isize = k.index32 != 0 ? 4u : 2u;
        if (res.indices.empty() || b->state == BlasState::Failed) {
            b->proxy.state = Blas::Proxy::State::Failed;  // the simplifier kept most of the mesh (nothing to gain): the original stays
            ++proxyJobsFailed_;
            continue;
        }
        const std::uint64_t bytes = (static_cast<std::uint64_t>(res.indices.size()) * isize + 3) & ~static_cast<std::uint64_t>(3);
        std::vector<std::uint8_t> data(bytes, 0);
        for (std::size_t i = 0; i < res.indices.size(); ++i) {
            if (isize == 4) {
                std::memcpy(data.data() + i * 4, &res.indices[i], 4);
            } else {
                const std::uint16_t v = static_cast<std::uint16_t>(res.indices[i]);
                std::memcpy(data.data() + i * 2, &v, 2);
            }
        }
        const rhi::RingAlloc up = dev_.ring(bytes, 16);
        std::unique_ptr<rhi::Buffer> buf;
        if (up.ptr != nullptr) {
            buf = rd->createBuffer({bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Device, "rt scene lod indices"});
        }
        if (buf == nullptr) {
            // the ring is full this frame: the Blas is queued again from its next instance (the job's result is lost)
            b->proxy = {};
            continue;
        }
        std::memcpy(up.ptr, data.data(), bytes);
        cl.copyBuffer(*buf, 0, dev_.game().ringBuffer(up), up.offset, bytes);
        copied = true;
        rhi::TriangleGeometry g;
        g.vertexAddress = b->vbAddress.plus(static_cast<std::int64_t>(k.vertexOffset) + static_cast<std::int64_t>(k.baseVertex) * k.stride).va;
        g.vertexStride = k.stride;
        g.vertexCount = b->maxVertex + 1;
        g.indexType = k.index32 != 0 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        g.indexAddress = buf->gpuAddress();
        g.indexCount = static_cast<std::uint32_t>(res.indices.size());
        g.opaque = true;
        rhi::BlasDesc desc;
        desc.geometries = std::span<const rhi::TriangleGeometry>(&g, 1);
        desc.allowCompaction = false;
        desc.name = "rt scene lod blas";
        std::unique_ptr<rhi::AccelerationStructure> as = rd->createBlas(desc);
        if (as == nullptr) {
            b->proxy.state = Blas::Proxy::State::Failed;
            ++proxyJobsFailed_;
            continue;
        }
        geoms.push_back(g);
        builds.push_back({as.get(), std::span<const rhi::TriangleGeometry>(&geoms.back(), 1), false});
        b->proxy.triangles = static_cast<std::uint32_t>(res.indices.size() / 3);
        b->proxy.bytes = as->storageBytes() + bytes;
        b->proxy.indices = std::move(buf);
        b->proxy.as = std::move(as);
        b->proxy.state = Blas::Proxy::State::Built;
        blasBytes_ += b->proxy.bytes;
        proxyBytes_ += b->proxy.bytes;
        ++proxiesBuilt_;
        proxyTrianglesOriginal_ += b->triangles;
        proxyTrianglesKept_ += b->proxy.triangles;
        ++proxyJobsDone_;
        proxyBuildMs_ += msSince(t0);
    }
    proxyJobs_ = std::move(keep);
    if (builds.empty()) return;
    if (copied) {
        cl.memoryBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    cl.buildBlases(builds);
}

bool RtScene::captureOptions(const d3d9::Shader& s, shader::TranslateOptions& o) {
    if (s.stage != shader::Stage::Vertex) return false;
    std::string tc = "float4(0.0, 0.0, 0.0, 0.0)";
    for (const auto& in : s.reflection.vertexInputs) {
        if (in.usage == shader::Usage::TexCoord && in.usageIndex == 0) tc = std::format("v{}", in.location);
    }
    // the previous frame's world position of the vertex (w = 1; 0 = unknown): per-object motion (enh/gbuffer), an extra varying
    o.extraOutputs = std::format("    [[vk::location({})]] float4 rvk_prevWorld : VARY{};", kPrevWorldLocation, kPrevWorldLocation);
    o.prelude = std::format(
        "// the capture buffer (RvkCapture): a 16-byte header (uint4: x slots, yz the previous frame's capture of the draw (0 = none), w its slots), then per slot two\n"
        "// float4: the world position (w = 1) and TEXCOORD0; read / written by address (vertex shader stores)\n"
        "// the operand the shader multiplies with its view-projection: the world position, in any view (TranslateOptions::dotCaptures)\n"
        "static float4 rvk_capW = float4(0.0, 0.0, 0.0, 0.0);\n");
    if (const auto& vp = s.semantics[d3d9::Semantic::ViewProjection]; vp.present()) {
        o.dotCaptures.push_back({static_cast<std::uint32_t>(vp.reg), "rvk_capW"});
    }
    o.epilogue = std::format(
        "    rvk_out.rvk_prevWorld = float4(0.0, 0.0, 0.0, 0.0);\n"
        "    {{\n"
        "        uint2 rvk_capAddr = rvkDrawPrevVs();\n"
        "        if (any(rvk_capAddr != uint2(0u, 0u))) {{\n"
        "            RvkAddr rvk_cap = rvkAddr(rvk_capAddr);\n"
        "            int rvk_slot = int(rvk_in.rvk_vertexId) - int(rvk_draw.alpha.w);\n"
        "            uint4 rvk_info = rvkRwLoad<uint4, 16>(rvk_cap);\n"
        "            uint rvk_slots = rvk_info.x & 0x7FFFFFFFu;\n"
        "            // the main view: the clip position back to the world; a cascade's view (bit 31 of the header's slots): the operand the\n"
        "            // shader multiplied with its view-projection\n"
        "            float4 rvk_w = (rvk_info.x & 0x80000000u) != 0u ? rvk_capW : mul(rvk_pos, rvkLoadFrame(pc.frame).invViewProj);\n"
        "            if (rvk_slot >= 0 && uint(rvk_slot) < rvk_slots && abs(rvk_w.w) > 1e-20) {{\n"
        "                float3 rvk_world = rvk_w.xyz / rvk_w.w;\n"
        "                uint rvk_at = rvk_in.rvk_instanceId * rvk_slots + uint(rvk_slot);  // instanced draws: one run of slots per instance\n"
        "                rvkRwStore<float4, 16>(rvkAddrAdd(rvk_cap, 16u + rvk_at * 32u), float4(rvk_world, 1.0));\n"
        "                rvkRwStore<float4, 16>(rvkAddrAdd(rvk_cap, 16u + rvk_at * 32u + 16u), {});\n"
        "                rvk_out.rvk_prevWorld = float4(rvk_world, 0.0);\n"
        "                if (any(rvk_info.yz != uint2(0u, 0u)) && uint(rvk_slot) < rvk_info.w) {{\n"
        "                    float4 rvk_prev = rvkRawLoad<float4, 16>(rvkAddrAdd(rvkAddr(rvk_info.yz), 16u + uint(rvk_slot) * 32u));\n"
        "                    if (rvk_prev.w == 1.0) rvk_out.rvk_prevWorld = float4(rvk_prev.xyz, 1.0);\n"
        "                }}\n"
        "            }}\n"
        "        }}\n"
        "    }}",
        tc);
    d3d9::Device::boneCompareText(o, s);  // (native.bone_motion=compare only: frame-ownership.md 14.13)
    return true;
}

// a blended main-view draw of a pack entry with transmission > 0 (glass): only while a glass consumer exists (exact off: nothing changes)
bool RtScene::isGlassDraw(const d3d9::ReplaceQuery& q) const {
    if (glassConsumers_.empty() || pack_ == nullptr || q.tag == nullptr || q.tag->material == nullptr) return false;
    const auto hit = pack_->lookupMaterial(q.tag->material);
    return hit.word != 0 && matpack::decodeWord(hit.word).glass;
}

bool RtScene::dynamicSupported() const { return supported() && dev_.game().gameCaps().vertexStores; }

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
            dev_.setVariantProvider(d3d9::VariantSlot::Capture, {d3d9::Device::captureTag(), captureOptions});
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
        animConsumers_[consumer] = rangeUnits;
    } else {
        animConsumers_.erase(consumer);
    }
    // every consumer's trees are captured: the range of the one that wants the most (RT shadows and GI ask for their own)
    animRange_ = 0.0f;
    for (const auto& [c, r] : animConsumers_) animRange_ = std::max(animRange_, r);
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
    const rhi::GpuAddr indices = b.key.strip != 0 ? addrOf(*b.listIndices) : b.ibAddress.plus(static_cast<std::int64_t>(b.key.first) * (index32 ? 4u : 2u));
    if (indices.null()) return;
    const std::uint32_t slots = b.maxVertex + 1;
    if (batch.base.null()) {
        // the draw's capture buffer: a header + `count` runs of `slots` vertices (an instanced batch's instances, whether or
        // not they are all close enough: instance k always writes run k)
        const rhi::RingAlloc a = dev.game().allocCapture(16 + static_cast<std::uint64_t>(slots) * 32 * count, 16);
        if (a.ptr == nullptr) {
            batch.failed = true;
            return;
        }
        const std::uint32_t header[4] = {slots, 0, 0, 0};
        std::memcpy(a.ptr, header, 16);
        batch.base = a.addr();
        batch.slots = slots;
        dev.requestVertexCapture(a, q.instanced, d3d9::Device::CaptureBy::RtTree, d3d9::Device::captureKindOf(q));
    }
    if (batch.slots != slots) return;
    DynBlas& db = dynBlas_[in.key];
    if (db.id == 0) {
        db.id = nextDynId_++;
        db.updates = static_cast<std::uint32_t>((in.key >> 8) % 30);  // refits / rebuilds of the trees do not all fall in one frame
    }
    DynDraw d{in.key, batch.base.plus(static_cast<std::int64_t>(index) * slots * 32), indices, slots, b.triangles, index32, false, true, b.id, batch.base.ref};
    dyn_.push_back(d);
    ++dynTrees_;
    ++iv_.treeCaptured;
}

// the vertex layout the skinning pass reads (rt_skin.comp): stream 0: POSITION F3, BLENDWEIGHT0 F4, BLENDINDICES0 UBYTE4, TEXCOORD0 F2; the morph targets
// (POSITION1..3 F3 in streams 1..3, the heads') are added with the vertex shader's weights c31.xyz (14.15 c: before, a draw with active weights kept the vertex shader
// capture, 27 - 72 solid draws per 5 s without a skinning job); `l.offsets` packs the four stream-0 byte offsets
bool RtScene::skinEligible(const d3d9::ReplaceQuery& q, const float* morphWeights, SkinLayout& l) const {
    l = SkinLayout{};
    if (q.decl == nullptr || q.vertexBuffer == nullptr || q.vertexStride == 0 || (q.vertexStride & 3u) != 0 || q.vertexStride > 255 ||
        q.vertexBuffer->ringCopied() || (q.vertexOffset & 3u) != 0) {
        return false;
    }
    int pos = -1, bw = -1, bi = -1, tc = -1;
    int morph[3] = {-1, -1, -1};
    for (const d3d9::VertexElement& e : q.decl->elements()) {
        if (e.stream == d3d9::kDeclEndStream) break;
        if (e.stream != 0) {
            // a morph target: POSITIONk F3 in stream k (the deltas of the normals are not needed: positions only)
            if (e.stream >= 1 && e.stream <= 3 && e.usage == 0 && e.usageIndex == e.stream) {
                if (e.type != 2) return false;
                morph[e.stream - 1] = e.offset;
            }
            continue;
        }
        if (e.usage == 0 && e.usageIndex == 0 && e.type == 2) pos = e.offset;
        else if (e.usage == 0) return false;  // (a morph target in stream 0: not a layout the game uses)
        else if (e.usage == 1 && e.usageIndex == 0 && e.type == 3) bw = e.offset;
        else if (e.usage == 2 && e.usageIndex == 0 && e.type == 5) bi = e.offset;
        else if (e.usage == 5 && e.usageIndex == 0 && e.type == 1) tc = e.offset;
    }
    for (int o : {pos, bw, bi, tc}) {
        if (o < 0 || o > 252 || (o & 3) != 0) return false;
    }
    // (c31 is the morph weights only for a layout with morph streams: other skinned shaders leave it to whatever the register file holds)
    const bool morphLayout = morph[0] >= 0 || morph[1] >= 0 || morph[2] >= 0;
    for (std::uint32_t k = 0; k < 3 && morphLayout; ++k) {
        const bool active = std::fabs(morphWeights[k]) > 1e-6f;
        if (morph[k] < 0) {
            if (active) return false;  // (weights for a target the layout does not have: the vertex shader reads something else)
            continue;
        }
        const d3d9::Buffer* b = q.morphBuffer[k];
        const std::uint32_t st = q.morphStride[k];
        if (b == nullptr || b->ringCopied() || st == 0 || (st & 3u) != 0 || st > 255 || (q.morphOffset[k] & 3u) != 0 || morph[k] > 252 || (morph[k] & 3) != 0) {
            if (active) return false;
            continue;
        }
        l.morphOff[k] = static_cast<std::uint32_t>(morph[k]);
        l.morphMask |= 1u << k;
    }
    l.offsets = static_cast<std::uint32_t>(pos) | (static_cast<std::uint32_t>(bw) << 8) | (static_cast<std::uint32_t>(bi) << 16) |
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
    SkinLayout skinLayout;
    const bool skin = skinWanted_ && !solid && skinReady() && skinEligible(q, dev.vsConstant(31), skinLayout);
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
    bool solidHasJob = false;
    if (solid && skinWanted_ && skinReady_) {
        // diagnostics: a solid-pass character draw without a skinning job of this frame (the pose of the previous frame is used for it)
        bool has = false;
        for (const DynDraw& d : dyn_) has = has || (d.key == key && d.skin);
        solidHasJob = has;
        ++(has ? iv_.solidWithSkin : iv_.solidNoSkin);
        // (F-VAR2: the skinning pass has this frame's pose of the character, nothing reads its capture: no request, so an owned solid draw stays owned)
        // (14.13: a bone-motion draw's motion needs no capture either: dev.boneMotionDraw)
        if (has && (!characterMotion_ || dev.boneMotionCovered()) && !skinVerify_) {
            ++iv_.solidCaptureSkipped;
            dev.noteCaptureAvoided(d3d9::Device::CaptureBy::RtSolidMotion, d3d9::Device::captureKindOf(q));
            return;
        }
        if (!has && skinLogged_.size() < 12 && skinLogged_.insert(key).second) {
            SkinLayout lo;
            spdlog::info("rt: skin: a solid-pass draw without a skinning job: entity {}, primitives {}, morph weights {:.3f} {:.3f} {:.3f}, eligible {}, maxIndex {}, jobs {}",
                         reinterpret_cast<std::uintptr_t>(q.tag != nullptr ? q.tag->entity : nullptr), q.primitives, dev.vsConstant(31)[0], dev.vsConstant(31)[1],
                         dev.vsConstant(31)[2], skinEligible(q, dev.vsConstant(31), lo), dynBlas_[key].maxIndex, skinJobs_.size());
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
    const rhi::GpuAddr ibAddress = ib->gpuAddress();
    if (ibAddress.null()) return;
    // the skinning pass reads the draw's static vertex buffer by address
    rhi::GpuAddr vertexAddress;
    if (skin) {
        q.vertexBuffer->gpu(q.vertexStride);
        const rhi::GpuAddr vb = q.vertexBuffer->gpuAddress();
        const std::int64_t at = static_cast<std::int64_t>(q.vertexOffset) + static_cast<std::int64_t>(q.baseVertex) * q.vertexStride;
        if (!vb.null() && at >= 0) vertexAddress = vb.plus(at);
        // 14.15 c: the morph targets' streams (the same base vertex); a target whose stream has no address is left out when its weight is 0, else no skinning job
        for (std::uint32_t k = 0; k < 3 && !vertexAddress.null(); ++k) {
            if ((skinLayout.morphMask >> k & 1u) == 0) continue;
            d3d9::Buffer* mb = q.morphBuffer[k];
            mb->gpu(q.morphStride[k]);
            const rhi::GpuAddr ma = mb->gpuAddress();
            const std::int64_t mat = static_cast<std::int64_t>(q.morphOffset[k]) + static_cast<std::int64_t>(q.baseVertex) * q.morphStride[k];
            if (!ma.null() && mat >= 0) {
                skinLayout.morph[k] = ma.plus(mat).ref;
            } else if (std::fabs(dev.vsConstant(31)[k]) > 1e-6f) {
                vertexAddress = {};
            } else {
                skinLayout.morphMask &= ~(1u << k);
            }
        }
    }
    const bool useSkin = skin && !vertexAddress.null();
    if (mainView && !solid && !useSkin) return;
    const rhi::RingAlloc a = dev.game().allocCapture(16 + static_cast<std::uint64_t>(slots) * 32, 16);
    if (a.ptr == nullptr) return;
    // header: slots, the previous frame's capture of this draw (its vertex shader reads the previous positions: motion)
    // (bit 31 of the slots: a cascade's view, whose vertex shader's world position operand is captured instead of the clip position)
    std::uint32_t header[4] = {slots | ((mainView || useSkin) ? 0u : 0x80000000u), 0, 0, 0};
    if (solid && !useSkin) {
        // native.bone_motion=compare (frame-ownership.md 14.13): the solid-pass capture is the motion source: its vertex shader compares the bone path too (bit 30)
        const std::uint32_t bit = dev.boneCompareBit();
        header[0] |= bit;
        if (bit != 0) ++dev.boneStats.compareDraws;
    }
    if (mainView && !useSkin && !b.lastCapture.null() && b.lastCaptureFrame + 1 == frame) {
        header[1] = b.lastCapture.low();
        header[2] = b.lastCapture.high();
        header[3] = b.lastSlots;
    }
    std::memcpy(a.ptr, header, 16);
    if (useSkin) {
        SkinJob j;
        j.vertices = vertexAddress.ref;
        j.out = a.rw();
        j.outKey = a.ref;
        j.slots = slots;
        j.stride = q.vertexStride;
        j.offsets = skinLayout.offsets;
        j.morphMask = skinLayout.morphMask;
        for (std::uint32_t k = 0; k < 3; ++k) {
            j.morph[k] = skinLayout.morph[k];
            j.morphOff[k] = skinLayout.morphOff[k];
            j.morphStride[k] = (skinLayout.morphMask >> k & 1u) != 0 ? q.morphStride[k] : 0u;
            j.morphWeight[k] = dev.vsConstant(31)[k];
        }
        if (j.morphMask != 0) ++iv_.skinMorph;
        // the bone matrices: c32 .. c231 of this draw's vertex shader constants
        const rhi::RingAlloc bones = dev.ring(200u * 16u, 16);
        if (bones.ptr == nullptr) return;
        std::memcpy(bones.ptr, dev.vsConstant(kInstanceDataBase), 200u * 16u);
        j.bones = bones.ref;
        if (skinVerify_) {
            // the vertex shader's own result for the same draw (the cascade view's capture variant), zeroed: slots it did not write have w = 0
            const rhi::RingAlloc ref = dev.game().allocCapture(16 + static_cast<std::uint64_t>(slots) * 32, 16);
            if (ref.ptr != nullptr) {
                std::uint32_t rh[4] = {slots | 0x80000000u, 0, 0, 0};
                std::memcpy(ref.ptr, rh, 16);
                std::memset(static_cast<std::uint8_t*>(ref.ptr) + 16, 0, static_cast<std::size_t>(slots) * 32);
                dev.requestVertexCapture(ref, false, d3d9::Device::CaptureBy::RtSkinVerify, d3d9::Device::captureKindOf(q));
                j.ref = ref.rw();
            }
        }
        skinJobs_.push_back(j);
    } else {
        // (census: a solid-pass draw without a skinning job of this frame, one kept for its motion, or a cascade's draw the skinning pass cannot take)
        const d3d9::Device::CaptureBy by = !solid ? d3d9::Device::CaptureBy::RtCascade : solidHasJob ? d3d9::Device::CaptureBy::RtSolidMotion : d3d9::Device::CaptureBy::RtSolidNoJob;
        dev.requestVertexCapture(a, false, by, d3d9::Device::captureKindOf(q));
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
    DynDraw d{key, a.addr(), ibAddress.plus(static_cast<std::int64_t>(q.first) * isize), slots, q.primitives, index32, false};
    d.main = mainView && !useSkin;
    d.skin = useSkin;
    dyn_.push_back(d);
}

// the frame's captured characters -> one BLAS each: refit while the topology holds (30 times), else a full build
void RtScene::buildDynamic(rhi::CommandList& cl, std::uint64_t frame) {
    animatedKeys_.clear();
    if (frame % 120 == 0) dropDynamic(false, frame);
    // Per key the newest capture that ran: this frame's (the cascades' views before the main view, all of them at AfterOpaque),
    // else the previous frame's (its main-view solid pass; its ring memory lives one frame more)
    dynUse_.clear();
    if (!dynamic_) return;
    if (dynFrame_ != frame) rollDyn(frame);
    recordSkin(cl);
    {
        std::unordered_set<std::uint64_t> taken;
        for (const DynDraw& d : dyn_) {
            if (taken.contains(d.key)) continue;
            const rhi::GpuRef asked = !d.requested.null() ? d.requested : d.capture.ref;
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
            const rhi::GpuRef asked = !d.requested.null() ? d.requested : d.capture.ref;
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
            const rhi::GpuRef asked = !d.requested.null() ? d.requested : d.capture.ref;
            if (!dev_.vertexCaptured(asked)) continue;
            DynBlas& b = dynBlas_[d.key];
            b.lastCapture = d.capture.ref;
            b.lastSlots = d.slots;
            b.lastCaptureFrame = frame;
        }
        for (const DynDraw& d : dynPrev_) {
            if (!d.main || d.tree) continue;
            DynBlas& b = dynBlas_[d.key];
            if (b.lastCaptureFrame >= frame - 1 && !b.lastCapture.null()) continue;
            const rhi::GpuRef asked = !d.requested.null() ? d.requested : d.capture.ref;
            if (!dev_.vertexCapturedPrev(asked)) continue;
            b.lastCapture = d.capture.ref;
            b.lastSlots = d.slots;
            b.lastCaptureFrame = frame - 1;
        }
    }
    if (dynUse_.empty()) return;
    rhi::Device* rd = dev_.rhi();
    // (the builds hold spans into `geoms`: no reallocation)
    std::vector<rhi::TriangleGeometry> geoms;
    std::vector<rhi::BlasBuild> builds;
    std::vector<std::size_t> which;
    std::vector<std::size_t> skippedRefits;
    geoms.reserve(dynUse_.size());
    builds.reserve(dynUse_.size());
    for (std::size_t i = 0; i < dynUse_.size(); ++i) {
        DynDraw& d = dynUse_[i];
        DynBlas& b = dynBlas_[d.key];
        rhi::TriangleGeometry g;
        g.vertexAddress = d.capture.plus(16).va;
        g.vertexStride = 32;
        g.vertexCount = d.slots;
        g.indexType = d.index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        g.indexAddress = d.indices.va;
        g.indexCount = d.triangles * 3;
        g.opaque = true;
        const bool sameShape = b.as != nullptr && b.sizeTriangles == d.triangles && b.sizeSlots == d.slots && b.sizeIndex32 == d.index32;
        const bool update = b.built && sameShape && b.indices == d.indices && b.updates < 30;
        // the wind moves slowly: a tree's BLAS is refitted every other frame (staggered by its key), the BLAS of the frame before
        // stands in (its instance still takes the capture of this frame for the materials' vertex reads); a refit costs the
        // driver about a microsecond of command recording each, 100 trees in range are the bulk of the pass
        if (update && d.tree && ((frame + (d.key >> 7)) & 1u) != 0) {
            skippedRefits.push_back(i);
            continue;
        }
        if (!sameShape) {
            // the structure is sized by the counts only: created again when they change (the old one goes with the frame being recorded)
            rhi::BlasDesc desc;
            desc.geometries = std::span<const rhi::TriangleGeometry>(&g, 1);
            desc.allowUpdate = true;
            desc.preferFastBuild = true;
            desc.name = "rt scene character";
            b.as = rd->createBlas(desc);
            b.built = false;
            if (b.as == nullptr) continue;
            b.sizeTriangles = d.triangles;
            b.sizeSlots = d.slots;
            b.sizeIndex32 = d.index32;
        }
        if (!update) {
            b.updates = 0;
        } else {
            ++b.updates;
        }
        geoms.push_back(g);
        builds.push_back({b.as.get(), std::span<const rhi::TriangleGeometry>(&geoms.back(), 1), update});
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
    if (builds.empty()) return;
    // the vertex shaders' captures (the cascades' and the solid pass's draws) and the skinning pass's output -> the builds
    barrier(cl, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
    {
        d3d9::RegionScope r(dev_, "rt.dynamic builds");
        cl.buildBlases(builds);
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
        rhi::Device* rdev = dev_.rhi();
        if (rdev == nullptr) {
            skinFailed_ = true;
            return false;
        }
        skinBuild_ = std::async(std::launch::async, [rdev]() {
            rhi::ComputePipelineDesc d;
            d.cs.spirv = shader::embedded::spirv("rt/rt_skin.comp");
            d.cs.dxil = shader::embedded::dxilOnDemand("rt/rt_skin.comp");
            d.name = "rt.skin";
            return rdev->createComputePipeline(d);
        });
        return false;
    }
    if (!skinBuild_.valid() || skinBuild_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    skinPipe_ = skinBuild_.get();
    if (!skinPipe_) {
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
    if (skinBuild_.valid()) skinBuild_.get();  // (a pipeline still being built is waited for; the RHI retires it with the device's frames)
    skinPipe_.reset();
    skinErr_.reset();
    skinReady_ = skinStarted_ = false;
}

// one dispatch for all of the frame's jobs (the frame's first skinned draws: z pre-pass and cascades), before the BLAS builds
void RtScene::recordSkin(rhi::CommandList& cl) {
    if (skinJobs_.empty()) return;
    if (!skinReady()) {
        skinJobs_.clear();
        return;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(skinJobs_.size(), 512));
    std::uint32_t chunks = 0;
    for (std::uint32_t i = 0; i < n; ++i) chunks += (skinJobs_[i].slots + 63) / 64;
    const rhi::RingAlloc t = dev_.ring(16 + 128ull * n, 16);  // (8 uint4 per job: rt_skin.comp.hlsl)
    if (t.ptr == nullptr) {
        skinJobs_.clear();
        return;
    }
    rhi::GpuRef errAddress;
    const std::uint32_t slot = dev_.frameSlot();
    if (skinVerify_) {
        if (skinErr_ == nullptr) {
            rhi::Device* rd = dev_.rhi();
            if (rd != nullptr) {
                skinErr_ = rd->createBuffer({16ull * rhi::kFramesInFlight, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::Memory::Readback, "rt skin verify"});
            }
            if (skinErr_ != nullptr) std::memset(skinErr_->map(), 0, 16ull * rhi::kFramesInFlight);
        }
        if (skinErr_ == nullptr) {
            skinVerify_ = false;
        } else if (skinErr_->map() != nullptr) {
            skinErr_->invalidate();
            auto* e = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(skinErr_->map()) + 16ull * slot);
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
            skinErr_->flush();
            const std::array<std::uint32_t, 2> er = skinErr_->shaderRef(16ull * slot);
            errAddress = rhi::GpuRef::fromWords(er[0], er[1]);
        }
    }
    auto* head = static_cast<std::uint32_t*>(t.ptr);
    head[0] = n;
    head[1] = chunks;
    head[2] = errAddress.low();
    head[3] = errAddress.high();
    std::uint32_t* jobs = head + 4;
    std::uint32_t chunk = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const SkinJob& j = skinJobs_[i];
        std::uint32_t* w = jobs + 32ull * i;
        w[0] = j.vertices.low();
        w[1] = j.vertices.high();
        w[2] = j.bones.low();
        w[3] = j.bones.high();
        w[4] = j.out.low();
        w[5] = j.out.high();
        w[6] = j.slots;
        w[7] = j.stride;
        w[8] = chunk;
        w[9] = 0;
        w[10] = j.ref.low();
        w[11] = j.ref.high();
        w[12] = j.offsets;
        w[13] = w[14] = w[15] = 0;
        // 14.15 c: the morph targets (rt_skin.comp.hlsl job words 4 .. 6)
        w[9] = j.morphMask;
        w[16] = j.morph[0].low();
        w[17] = j.morph[0].high();
        w[18] = j.morph[1].low();
        w[19] = j.morph[1].high();
        w[20] = j.morph[2].low();
        w[21] = j.morph[2].high();
        w[22] = j.morphStride[0] | (j.morphStride[1] << 8) | (j.morphStride[2] << 16);
        w[23] = j.morphOff[0] | (j.morphOff[1] << 8) | (j.morphOff[2] << 16);
        std::memcpy(&w[24], j.morphWeight, 3 * sizeof(float));
        w[27] = 0;
        w[28] = w[29] = w[30] = w[31] = 0;
        chunk += (j.slots + 63) / 64;
        skinVerticesTotal_ += j.slots;
    }
    {
        d3d9::RegionScope r(dev_, "rt.skin");
        // a reference capture written by a cascade's vertex shader (verify) before the compute reads it
        barrier(cl, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        dev_.game().captureAccess(cl, true);  // (D3D12: the capture pool in its write state)
        cl.bindPipeline(*skinPipe_);
        abi::SkinPush push{};
        push.h.frame = 0;
        push.table[0] = t.ref.low();
        push.table[1] = t.ref.high();
        cl.setPushConstants(&push, sizeof(push));
        cl.dispatch(chunks, 1, 1);
        dev_.game().captureAccess(cl, false);
    }
    for (std::uint32_t i = 0; i < n; ++i) dev_.markVertexCaptured(skinJobs_[i].outKey);
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
    for (auto it = dynBlas_.begin(); it != dynBlas_.end();) {
        DynBlas& b = it->second;
        if (!all && b.lastFrame + 120 >= frame) {
            ++it;
            continue;
        }
        if (b.material != 0) materials_.release(b.material);
        it = dynBlas_.erase(it);  // (its structure goes with the frame being recorded)
    }
    if (all) {
        dyn_.clear();
        dynPrev_.clear();
        dynUse_.clear();
    }
}

// ---- the pass ----------------------------------------------------------------------------------------------------------------

void RtScene::record(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !active_ || failed_) return;
    if (builtFrame_ == dev_.frameValue()) return;  // built at the z pre-pass already
    builtFrame_ = dev_.frameValue();
    if (!ensureResources()) return;
    const auto t0 = std::chrono::steady_clock::now();
    d3d9::Device& frames = dev_;
    const std::uint32_t slot = frames.frameSlot();
    const std::uint64_t frame = dev_.frameValue();
    const double now = dev_.secondsSinceCreate();
    RhiScope rs(dev_, ctx.cmd);
    if (!rs) return;
    rhi::CommandList& cl = rs.cl();
    // (the barriers before every build and after it, also the one that protects the scratch pool and the TLAS of this slot from earlier
    // submissions' builds and ray queries, are the RHI's: CommandList::buildBlases / buildTlasAt)

    // the camera of this frame's main view
    const auto& cam = dev_.camera().valid ? dev_.camera() : dev_.lastCamera();
    const float camera[3] = {cam.position[0], cam.position[1], cam.position[2]};
    tableMaintain(frame);
    registry_.age(camera, frame, now, limits_, [this](std::uint64_t id) -> const rt::Aabb* {
        const Blas* b = findBlas(id);
        return b != nullptr && b->bounds.valid() ? &b->bounds : nullptr;
    });
    evictOverBudget();
    if (budgetBytes_ != 0 && frame % 16 == 0) {
        // the memory limit (rt.memory_budget): over it the ladder tightens and far originals are given up; well under it it relaxes again
        const std::uint64_t total = asBytes();
        if (total > budgetBytes_) {
            budgetScale_ = std::max(0.08f, budgetScale_ * 0.85f);
            applyLodEffective();
            releaseOriginals(budgetBytes_ / 10 * 9);
        } else if (total < budgetBytes_ / 10 * 8 && budgetScale_ < 1.0f) {
            budgetScale_ = std::min(1.0f, budgetScale_ * 1.05f);
            applyLodEffective();
        }
    }
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
        processCompactions(cl, slot);
    }
    {
        d3d9::RegionScope r(dev_, "rt.blas builds");
        buildPending(cl, slot);
    }
    if ((lodRangeUnits_ > 0.0f && lodBias_ > 0) || !proxyJobs_.empty()) {
        d3d9::RegionScope r(dev_, "rt.lod proxies");
        buildProxies(cl);
    }
    {
        d3d9::RegionScope r(dev_, "rt.dynamic");
        buildDynamic(cl, frame);
    }

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
            return b != nullptr && !in.pinned && b->volatileUntil >= frame && in.lastFrame + 4 * kVolatileKeep < frame;
        });
    }
    // level of detail (docs/research/rt-lod.md): each instance's rung (0 the engine's draw, 1 the proxy, 2 the box) with hysteresis,
    // from its distance; only static opaque instances of a built BLAS whose per-triangle materials are not needed move off rung 0
    const bool lodOn = lodActive() && cam.valid;
    nearBlas_.clear();
    const float proxyRange = lodRangeUnits_ > 0.0f && lodBias_ > 0 ? lodRangeUnits_ : 0.0f;
    if (lodOn) {
        const float nearest = std::min(proxyRange > 0.0f ? proxyRange : 3.0e38f, lodFarUnits_ > 0.0f ? lodFarUnits_ : 3.0e38f) * 0.8f;
        for (rt::Registry::Instance& in : registry_.instancesMutable()) {
            if (!in.sphereValid) {
                in.lod = 0;
                nearBlas_.insert(in.blas);
                continue;
            }
            const float dx = in.sphere.c[0] - camera[0], dy = in.sphere.c[1] - camera[1], dz = in.sphere.c[2] - camera[2];
            const float d = std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - in.sphere.r);
            if (in.lod == 0 && d < nearest) {
                nearBlas_.insert(in.blas);
                if (released_ != 0) {
                    if (Blas* rb = findBlas(in.blas); rb != nullptr && rb->released) requestRebuild(*rb);
                }
                continue;
            }
            Blas* b = findBlas(in.blas);
            if (b == nullptr || !lodEligible(*b) || (in.flags & abi::kRtRecordAlphaTested) != 0) {
                in.lod = 0;
                nearBlas_.insert(in.blas);
                continue;
            }
            const RtMaterial* m = in.material != 0 ? materials_.get(in.material) : nullptr;
            if (m == nullptr || m->dead) m = bestMaterial(*b);
            if (m != nullptr && m->masked) {
                in.lod = 0;
                nearBlas_.insert(in.blas);
                continue;
            }
            // a box only for bounded objects (a cliff, the terrain or a level mesh is no box)
            const bool boxOk = lodFarUnits_ > 0.0f && b->bounds.valid() && in.sphere.r <= kBoxMaxRadius && b->triangles >= kBoxMinTriangles;
            in.lod = static_cast<std::uint8_t>(rt::lod::rungFor(in.lod, d, proxyRange, boxOk ? lodFarUnits_ : 0.0f));
            if (in.lod == 2 && !boxOk) in.lod = proxyRange > 0.0f ? 1 : 0;
            if (in.lod == 0) {
                nearBlas_.insert(in.blas);
                if (b->released) requestRebuild(*b);
            }
        }
    }
    if (!lodOn && released_ != 0) {
        // the ladder is off (or no camera): every released original comes back
        for (auto& [id, rb] : blas_) {
            if (rb.released) requestRebuild(rb);
        }
    }
    const std::vector<std::uint32_t> chosen = rt::selectForTlas(list, camera, tlas_[slot].capacity - dynCount, [this, frame, &camera](const rt::Registry::Instance& in) {
        const Blas* b = findBlas(in.blas);
        // an object the cascades draw at another level of detail than the main view: the main view's
        if (b == nullptr || (b->state != BlasState::Built && !b->rebuild) || registry_.supersededBy(in, viewBit(View::Main))) return false;
        if (b->animatedOnly) return false;
        // far away and tiny: not part of the scene (rung 2 only)
        if (in.lod == 2 && in.sphereValid) {
            const float dx = in.sphere.c[0] - camera[0], dy = in.sphere.c[1] - camera[1], dz = in.sphere.c[2] - camera[2];
            if (rt::lod::tooSmall(in.sphere.r, std::sqrt(dx * dx + dy * dy + dz * dz))) {
                ++lastSkippedTiny_;
                return false;
            }
        }
        // an animated SpeedTree stands in for its rest-pose instance (the wind: observeTree)
        if (!animatedKeys_.empty() && animatedKeys_.contains(in.key)) return false;
        // volatile ranges (churning instances): only what was seen in the last frames, the stale ones are ghosts of earlier places
        if (b->volatileUntil >= frame && !in.pinned && in.lastFrame + kVolatileKeep < frame) return false;
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
    const rhi::RingAlloc ia = frames.ring(std::max<std::uint64_t>(n, 1) * sizeof(VkAccelerationStructureInstanceKHR), 16);
    const rhi::RingAlloc ra = frames.ring(std::max<std::uint64_t>(n, 1) * sizeof(abi::RtGeometryRecord), 16);
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
    std::uint64_t textured = 0, untextured = 0, cutOut = 0, segmented = 0, speedTrees = 0, glassCount = 0;
    std::vector<abi::RtMaterialSegment> segs;              // the frame's segment table
    std::vector<std::pair<std::uint32_t, std::uint32_t>> segRecords;  // record -> its first segment
    // level of detail: instances beyond the range are traced through their BLAS's proxy; the stats snapshot by distance band
    const bool snap = std::chrono::steady_clock::now() - lastLog_ >= std::chrono::seconds(5);
    std::unordered_map<std::uint64_t, std::uint8_t> nearestBand;
    std::uint64_t characterTriangles = 0;
    std::uint64_t animatedTreeTriangles = 0;
    if (snap) {
        for (BandStat& s : bands_) s = {};
        bandsValid_ = true;
    }
    auto& mutableList = registry_.instancesMutable();
    for (std::uint32_t k = 0; k < worldCount; ++k) {
        rt::Registry::Instance& in = mutableList[chosen[k]];
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
        const bool glass = (in.flags & abi::kRtRecordGlass) != 0 && !glassConsumers_.empty();
        glassCount += glass ? 1 : 0;
        vi.mask = glass ? abi::kRtMaskGlass : alpha ? abi::kRtMaskAlphaTested : abi::kRtMaskOpaque;
        vi.instanceShaderBindingTableRecordOffset = 0;
        vi.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                   (alpha ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
        vi.accelerationStructureReference = b.as != nullptr ? b.as->address() : 0;  // (released: replaced below by the proxy or the box)
        insts[k] = vi;
        abi::RtGeometryRecord r{};
        const rt::BlasKey& key = b.key;
        split(b.vbAddress.plus(static_cast<std::int64_t>(key.vertexOffset) + static_cast<std::int64_t>(key.baseVertex) * key.stride).ref, r.vertices);
        split(key.strip != 0 ? addrOf(*b.listIndices).ref : b.ibAddress.plus(static_cast<std::int64_t>(key.first) * (key.index32 != 0 ? 4u : 2u)).ref,
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
        r.alpha = recordAlpha(in.flags, in.data, m);
        r.albedoTex = mf.albedoTex;
        r.albedoSampler = mf.albedoSampler;
        r.packWord = mf.packWord;
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
                    e.packWord = sf.packWord;
                    std::memcpy(e.uvMap, sf.uvMap, sizeof(e.uvMap));
                    segs.push_back(e);
                }
                r.segmentCount = static_cast<std::uint32_t>(b.segments.size());
                r.flags |= abi::kRtRecordSegmented;
                ++segmented;
            }
        }
        // the distance of the instance's bounding sphere from the camera (world units)
        float distance = 0.0f;
        if (in.sphereValid && cam.valid) {
            const float dx = in.sphere.c[0] - camera[0], dy = in.sphere.c[1] - camera[1], dz = in.sphere.c[2] - camera[2];
            distance = std::max(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - in.sphere.r);
        } else if (cam.valid) {
            const float dx = in.rows[3] - camera[0], dy = in.rows[7] - camera[1], dz = in.rows[11] - camera[2];
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        std::uint32_t instTriangles = b.triangles;
        bool proxied = false, boxed = false;
        const bool noOriginal = b.as == nullptr;
        if (lodOn && (in.lod != 0 || noOriginal)) {
            std::uint8_t rung = in.lod;
            if (noOriginal) rung = rung == 1 && b.proxy.state == Blas::Proxy::State::Built ? 1 : 2;  // coming back / released: whatever stands in
            if ((alpha || r.segmentCount != 0) && !noOriginal) {
                in.lod = 0;  // (cut-outs and per-triangle materials index the original's triangles)
            } else if (rung == 1) {
                if (b.proxy.state == Blas::Proxy::State::None) queueProxy(b);
                if (b.proxy.state == Blas::Proxy::State::Built && b.proxy.level == static_cast<std::uint32_t>(lodBias_)) {
                    insts[k].accelerationStructureReference = b.proxy.as->address();
                    split(addrOf(*b.proxy.indices).ref, r.indices);
                    instTriangles = b.proxy.triangles;
                    proxied = true;
                }
            } else if (ensureBox(cl)) {
                // the object's oriented bounding box: the unit cube scaled to the object-space AABB (shrunk a little; a tree's thin
                // hull: its longest axis is up) under the instance's rows: A' = A diag(h), t' = A c + t
                const rt::Aabb& bb = b.bounds;
                float c[3], h[3];
                int tall = 0;
                for (int a2 = 0; a2 < 3; ++a2) {
                    c[a2] = 0.5f * (bb.min[a2] + bb.max[a2]);
                    h[a2] = 0.5f * (bb.max[a2] - bb.min[a2]) * kBoxShrink;
                    if (bb.max[a2] - bb.min[a2] > bb.max[tall] - bb.min[tall]) tall = a2;
                }
                if ((in.flags & abi::kRtRecordSpeedTree) != 0) {
                    for (int a2 = 0; a2 < 3; ++a2) h[a2] *= a2 == tall ? 1.0f : kTreeThin;
                }
                auto& mat = insts[k].transform.matrix;
                for (int row = 0; row < 3; ++row) {
                    const float* A = &in.rows[row * 4];
                    for (int col = 0; col < 3; ++col) mat[row][col] = A[col] * h[col];
                    mat[row][3] = A[0] * c[0] + A[1] * c[1] + A[2] * c[2] + A[3];
                }
                insts[k].accelerationStructureReference = box_.as->address();
                const rhi::GpuAddr base = addrOf(*box_.buffer);
                split(base.ref, r.vertices);
                split(base.plus(96).ref, r.indices);
                r.stride = 12;
                r.flags &= ~(abi::kRtRecordIndex32 | abi::kRtRecordAlphaTested | abi::kRtRecordSpeedTree | abi::kRtRecordInstanced);
                r.normal = r.texcoord0 = r.texcoord1 = r.texcoord6 = r.color0 = abi::kRtAttribAbsent;
                r.albedoTex = 0;
                r.alpha = 8u;
                r.uvSet = abi::kRtUvNoTexture | abi::kRtUvOpacityOpaque;
                std::memset(r.uvMap, 0, sizeof(r.uvMap));
                const std::uint32_t colour = 0xFF5C6166u;  // a neutral stone / earth grey-brown (the box has no texture coordinates)
                std::memcpy(&r.uvMap[0][3], &colour, 4);
                instTriangles = 12;
                boxed = true;
            }
        }
        recs[k] = r;
        triangles += instTriangles;
        if (snap) {
            const std::size_t band = rt::lod::bandOf(distance / 100.0f);
            BandStat& s = bands_[band];
            const std::uint32_t kind = (in.data >> 16) & 0xFFu;
            const std::size_t cat = kind == static_cast<std::uint32_t>(Kind::StaticMesh) || kind == static_cast<std::uint32_t>(Kind::InstancedMesh) ? 0
                                    : kind == static_cast<std::uint32_t>(Kind::SpeedTree)                                                         ? 1
                                                                                                                                                  : 2;
            ++s.instances;
            s.triangles += instTriangles;
            s.trianglesOriginal += b.triangles;
            s.proxied += proxied ? 1 : 0;
            s.boxed += boxed ? 1 : 0;
            s.byKind[cat] += instTriangles;
            auto [it, fresh] = nearestBand.try_emplace(in.blas, static_cast<std::uint8_t>(band));
            if (!fresh) it->second = std::min<std::uint8_t>(it->second, static_cast<std::uint8_t>(band));
        }
        if (content != nullptr) content->push_back({in.blas, in.id, r.flags, in.material, {in.rows[3], in.rows[7], in.rows[11]}});
    }
    if (snap) {
        for (const auto& [id, band] : nearestBand) {
            const Blas* sb = findBlas(id);
            if (sb == nullptr) continue;
            bands_[band].blases += 1;
            bands_[band].blasBytes += (sb->as != nullptr ? sb->as->storageBytes() : 0) + (sb->listIndices != nullptr ? sb->listIndices->desc().size : 0) + sb->proxy.bytes;
        }
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
            vi.accelerationStructureReference = b.as->address();
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
                split(d.capture.plus(16).ref, r.vertices);
                split(d.indices.ref, r.indices);
                r.stride = 32;
                r.flags = (in->flags & ~abi::kRtRecordAlphaTested) | (alpha ? abi::kRtRecordAlphaTested : 0u) | (((in->data >> 16) & 0xFFu) << abi::kRtRecordKindShift);
                r.normal = abi::kRtAttribAbsent;
                r.texcoord0 = 16u | (1u << 16);  // FLOAT2 at 16: the capture's TEXCOORD0
                r.texcoord1 = r.texcoord0;
                r.texcoord6 = abi::kRtAttribAbsent;
                r.color0 = abi::kRtAttribAbsent;
                r.instanceId = in->id;
                r.alpha = recordAlpha(in->flags, in->data, m);
                r.albedoTex = mf.albedoTex;
                r.albedoSampler = mf.albedoSampler;
                r.uvSet = mf.uvSet;
                std::memcpy(r.uvMap, mf.uvMap, sizeof(r.uvMap));
                recs[k] = r;
                triangles += d.triangles;
                animatedTreeTriangles += d.triangles;
                ++treesNow;
                if (content != nullptr) content->push_back({d.blas, in->id, r.flags, in->material, {in->rows[3], in->rows[7], in->rows[11]}});
                ++k;
                continue;
            }
            insts[k] = vi;
            abi::RtGeometryRecord r{};
            split(d.capture.plus(16).ref, r.vertices);
            split(d.indices.ref, r.indices);
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
            characterTriangles += d.triangles;
            if (content != nullptr) content->push_back({0, b.id, r.flags, b.material, {0.0f, 0.0f, 0.0f}});
            ++k;
        }
        iv_.dynamic = k - worldCount;
        iv_.animatedTrees = treesNow;
        used = k;
    }

    // the segment table (after the records: their addresses)
    if (!segs.empty()) {
        const rhi::RingAlloc sa = frames.ring(segs.size() * sizeof(abi::RtMaterialSegment), 16);
        if (sa.ptr != nullptr) {
            std::memcpy(sa.ptr, segs.data(), segs.size() * sizeof(abi::RtMaterialSegment));
            for (const auto& [rec, first] : segRecords) split(sa.ref.plus(static_cast<std::int64_t>(first) * sizeof(abi::RtMaterialSegment)), recs[rec].segments);
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
    {
        d3d9::RegionScope r(dev_, "rt.tlas build");
        cl.buildTlasAt(*t.as, ia.gpuVa, used);  // (ends with the barrier to the ray queries of this submission)
    }
    if (used < n) {
        // (see `used`) counted for the log; before this was fixed the TLAS was built over n instances, the tail of them stale
        // ring memory: VK_ERROR_DEVICE_LOST after a teleport with shadows.rt on (docs/research/rt-shadows.md 9)
        ++iv_.holeFrames;
        iv_.holes += n - used;
    }
    dev_.setRtScene({t.as->address(), ra.ref, t.slot, used, used, frame});

    lastInstances_ = used;
    lastTriangles_ = triangles;
    lastCharTriangles_ = characterTriangles;
    lastAnimTreeTriangles_ = animatedTreeTriangles;
    iv_.textured = textured;
    iv_.untextured = untextured;
    iv_.cutOut = cutOut;
    glassInstances_ = glassCount;
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
    const double tlasMiB = tlas_[0].as != nullptr ? static_cast<double>(tlas_[0].as->storageBytes() * rhi::kFramesInFlight) / 1048576.0 : 0.0;
    const double blasMiB = static_cast<double>(blasBytes_) / 1048576.0;
    const double scratchMiB = dev_.rhi() != nullptr ? static_cast<double>(dev_.rhi()->buildScratchBytes()) / 1048576.0 : 0.0;
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
                 std::string("-PerfFrame: pass rt.scene"));
    logTable(f);
    const RtMaterials::Counters mc = materials_.takeCounters();
    spdlog::info("rt: scene: materials {} (per frame: {:.1f} resolves, {:.3f} ms; interval: {} uv evaluations, graph {} (pool image {}) / bytecode s9 {} / "
                 "no texture {}, approximate {}, failed {}, active shader != material's {}); records: textured {}, untextured {}, cut-out {}, segmented {} ({} segments; "
                 "engine sub-mesh segments {}, merged ranges resolved {}), SpeedTree {}",
                 materials_.size(), static_cast<double>(mc.resolves) / f, mc.ms / f, mc.evaluations, mc.graph, mc.pool, mc.heuristic, mc.noTexture,
                 mc.approx, mc.failed, mc.shaderMismatch, iv_.textured, iv_.untextured, iv_.cutOut, iv_.segmented, iv_.segments,
                 iv_.engineSegments, iv_.engineMerged, iv_.speedTree);
    if (bandsValid_) {
        // the last frame's TLAS by distance band (docs/research/rt-lod.md): instances, triangles traced (original in brackets when a
        // proxy stands in), by kind (meshes / SpeedTree branches / other), the BLAS storage whose nearest instance lies in the band
        std::string line;
        std::uint64_t totalTri = 0, totalOrig = 0;
        for (std::size_t i = 0; i < rt::lod::kBands; ++i) {
            const BandStat& s = bands_[i];
            totalTri += s.triangles;
            totalOrig += s.trianglesOriginal;
            line += std::format(" | {}: {} inst, {}k tri{} (mesh {}k, tree {}k, other {}k), {} proxied, {} boxed, {} BLAS {:.1f} MiB", rt::lod::bandName(i), s.instances,
                                s.triangles / 1000, s.trianglesOriginal != s.triangles ? std::format(" [{}k]", s.trianglesOriginal / 1000) : std::string(),
                                s.byKind[0] / 1000, s.byKind[1] / 1000, s.byKind[2] / 1000, s.proxied, s.boxed, s.blases, static_cast<double>(s.blasBytes) / 1048576.0);
        }
        spdlog::info("rt: scene: lod bands (proxy {:.0f} m, bias {}, box {:.0f} m){} | tiny skipped {} per frame | memory limit {} MB (scale {:.2f}, {} originals released, {} released / {} rebuilt in total, AS {:.1f} MiB) | total {}k tri [{}k], characters {}k tri, animated trees {}k tri | proxies: {} built "
                     "({} MiB, {} triangles of {} kept, {} jobs failed / nothing to gain, gather {:.1f} ms, build {:.1f} ms in total, {} queued)",
                     lodRangeUnits_ / 100.0f, lodBias_, lodFarUnits_ / 100.0f, line, lastSkippedTiny_ / std::max<std::uint64_t>(1, iv_.frames), budgetBytes_ >> 20, budgetScale_, released_, releasedTotal_, rebuiltTotal_, static_cast<double>(asBytes()) / 1048576.0, totalTri / 1000, totalOrig / 1000, lastCharTriangles_ / 1000, lastAnimTreeTriangles_ / 1000,
                     proxiesBuilt_, proxyBytes_ >> 20, proxyTrianglesKept_, proxyTrianglesOriginal_, proxyJobsFailed_, proxyGatherMs_, proxyBuildMs_, proxyJobs_.size());
        bandsValid_ = false;
        lastSkippedTiny_ = 0;
    }
    if (dynamic_) {
        spdlog::info("rt: scene: characters {} + animated SpeedTrees {} in the last TLAS ({} tracked, interval: {} BLAS builds, {} refits; "
                     "volatile ranges {}; tree draws {} ({} beyond the range, {} captured); BLAS refits from this frame's capture {} / the previous frame's {}; characters: {} from this frame's pose ({} skinned by compute) / {} from the previous frame's (skinning pass: {} draws taken, {} not eligible; solid-pass draws {} with / {} without a job, {} without a capture request); capture variant needs vertexPipelineStoresAndAtomics: {})",
                     iv_.dynamic - iv_.animatedTrees, iv_.animatedTrees, dynBlas_.size(), dynBuilt_, dynRefit_, iv_.volatileBlas, iv_.treeDraws, iv_.treeFar, iv_.treeCaptured, iv_.capCurrent, iv_.capPrevious,
                     iv_.charCurrent, iv_.charSkinned, iv_.charPrevious, iv_.skinDraws, iv_.skinIneligible, iv_.solidWithSkin, iv_.solidNoSkin, iv_.solidCaptureSkipped, dev_.game().gameCaps().vertexStores ? "yes" : "no");
        dynBuilt_ = dynRefit_ = 0;
    }
    if (iv_.holes > 0) {
        spdlog::info("rt: scene: TLAS built over fewer instances than counted in {} frames of the interval ({} dynamic instances dropped out: their registry instance or BLAS was gone)",
                     iv_.holeFrames, iv_.holes);
    }
    treeSetClose(dev_);
    if (!treeSets_[0].empty() || !treeSets_[1].empty()) {
        std::uint64_t frames[2] = {0, 0}, shared = 0;
        for (int k = 0; k < 2; ++k) {
            for (const auto& [id, n] : treeSets_[k]) {
                frames[k] += n;
                if (k == 1 && treeSets_[0].contains(id)) ++shared;
            }
        }
        spdlog::info("rt: scene: SpeedTree sightings (main solid pass, per frame): composed frames {} ({} distinct sets), owned frames {} ({} distinct sets), sets seen in both {} (a still camera: 1 / 1 / 1)", frames[0],
                     treeSets_[0].size(), frames[1], treeSets_[1].size(), shared);
        treeSets_[0].clear();
        treeSets_[1].clear();
    }
    iv_ = {};
    lastLog_ = now;
}

}  // namespace rvk::enh
