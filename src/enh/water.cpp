#include "enh/water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "shader/embedded.h"
#include "vk/formats.h"
#include "vk/pipeline.h"

namespace rvk::enh {

namespace draws = d3d9::draws;
using d3d9::DrawReplacement;

namespace {

// the engine's sea plane spans the world (Levelmesh_Oceanmesh_01: 672 x 572 km... in engine units 672,672 x 572,545); the
// largest inland water (Levelmesh_Lakemesh_02) is 20,833 wide, the island ~110,000: a water surface wider than this
// reaches the world's border, i.e. it is the open sea (docs/research/water.md 3)
constexpr float kSeaExtent = 200000.0f;
constexpr float kFlatSlack = 1000.0f;  // the sea plane is flat (its box is 2 units high)

// materials forced to the open sea regardless of their geometry (the override of the geometric signal; empty: Risen's own
// sea plane needs none)
constexpr const char* kSeaOverrides[] = {""};

// base values of the sea (world units = cm)
constexpr float kWaveHeight = 70.0f;   // amplitude of the largest wave at water.wave_height 1
constexpr float kNearCap = 30.0f;      // wave height near the camera (the player wades / swims there)
constexpr float kLowering = 16.0f;     // the waterline at the coast below the engine's
constexpr float kSwash = 10.0f;        // swash amplitude (< kLowering: the water never climbs above the engine's line)
constexpr float kCoastBand = 1800.0f;  // how far from the coast overlay the lowering / swash / coast foam reach
constexpr float kGridSpacing = 25.0f;  // finest grid spacing
constexpr int kGridLevels = 8;

std::uint64_t nameHash(const char* s) {
    std::uint64_t h = 1469598103934665603ull;
    for (; *s != 0; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
    return h;
}

void copy4(float* dst, const float* src) { std::memcpy(dst, src, 16); }

}  // namespace

Water::Water(d3d9::Device& device) : dev_(device) {
    // the shaders exist from the start, so their pipelines are built in the background at the session's first water draw
    // (also with the feature off): turning it on later costs no frame
    if (ensureShaders()) {
        dev_.registerReplacement(meshVs_, ps_, false);
        dev_.registerReplacement(oceanVs_, ps_, true);
    }
}

Water::~Water() {
    dev_.setDrawReplacer(nullptr);
    if (bake_.valid()) bake_.wait();
    if (field_) dev_.release(std::move(field_));
    destroyShaders();
}

void Water::destroyShaders() {
    vk::Context* ctx = &dev_.ctx();
    for (auto* s : {&meshVs_, &oceanVs_, &ps_}) {
        if (*s && (*s)->module != VK_NULL_HANDLE) {
            const VkShaderModule m = (*s)->module;
            dev_.frames().retire([ctx, m] { vkDestroyShaderModule(ctx->device(), m, nullptr); });
        }
        s->reset();
    }
}

bool Water::ensureShaders() {
    if (failed_) return false;
    if (meshVs_ && oceanVs_ && ps_) return true;
    const auto make = [&](const char* name, shader::Stage stage) {
        auto s = std::make_shared<d3d9::Shader>();
        s->stage = stage;
        s->module = vk::createShaderModule(dev_.ctx(), shader::embedded::spirv(name));
        s->hash = nameHash(name);
        return s;
    };
    using shader::Usage;
    meshVs_ = make("water/water_mesh.vert", shader::Stage::Vertex);
    meshVs_->reflection.vertexInputs = {{0, Usage::Position, 0}, {1, Usage::Normal, 0}, {2, Usage::TexCoord, 0}, {3, Usage::TexCoord, 4}};
    meshVs_->reflection.floatConstCount = 256;  // the whole VS pool: camera c0..c11, instance data c32+
    oceanVs_ = make("water/water_ocean.vert", shader::Stage::Vertex);
    oceanVs_->reflection.floatConstCount = 256;
    ps_ = make("water/water.frag", shader::Stage::Pixel);
    ps_->reflection.floatConstCount = 1;
    ps_->reflection.colorOutputs = 1;
    if (meshVs_->module == VK_NULL_HANDLE || oceanVs_->module == VK_NULL_HANDLE || ps_->module == VK_NULL_HANDLE) {
        spdlog::error("enh: water: shader modules failed, enhanced water stays off this session");
        failed_ = true;
        destroyShaders();
        return false;
    }
    return true;
}

void Water::prebuild() {
    if (prebuilt_ || !ensureShaders()) return;
    const bool a = dev_.prebuildReplacement(*meshVs_, *ps_, false);
    const bool b = dev_.prebuildReplacement(*oceanVs_, *ps_, true);
    prebuilt_ = a && b;
    spdlog::info("enh: water: pipelines {}", prebuilt_ ? "ready or building in the background"
                                                         : "built in the background at the first water draw");
}

void Water::apply(const Settings& s) {
    const bool was = settings_.enabled;
    settings_ = s;
    if (s.enabled) {
        if (!was) {
            dev_.setDrawReplacer(this);
            loggedFirst_ = false;
        }
        prebuild();
    } else if (was) {
        dev_.setDrawReplacer(nullptr);
    }
    spdlog::info("enh: water: {} (quality {}, reflections {}, ocean waves {}, wave height {:.2f}, murkiness {:.2f}, reflection "
                 "strength {:.2f}, debug {})",
                 s.enabled ? "on" : "off", s.quality, s.ssr ? "ssr" : "sky cube", s.oceanWaves, s.waveHeight, s.murkiness, s.reflections,
                 s.debug);
}

bool Water::worldRows(d3d9::Device& device, const d3d9::ReplaceQuery& q, float rows[3][4]) const {
    if (q.instanced) return false;
    for (int r = 0; r < 3; ++r) copy4(rows[r], device.vsConstant(33 + static_cast<std::uint32_t>(r)));
    return true;
}

// The body's geometry, once per vertex buffer: its world box decides whether it is the open sea (the engine's sea plane
// reaches the world's border; lakes, rivers, ponds and puddles never do, whatever their material is called)
const Water::Body& Water::body(d3d9::Device& device, const d3d9::ReplaceQuery& q) {
    static const Body none{};
    if (q.vertexBuffer == nullptr || q.vertexStride < 12) return none;
    const std::uint64_t id = q.vertexBuffer->ref().id;
    if (auto it = bodies_.find(id); it != bodies_.end()) return it->second;
    Body b;
    float rows[3][4];
    if (worldRows(device, q, rows)) {
        const std::uint32_t stride = q.vertexStride;
        const std::uint32_t count = std::min<std::uint32_t>((q.vertexBuffer->size() - std::min(q.vertexOffset, q.vertexBuffer->size())) / stride,
                                                            4u << 20);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(count) * stride);
        if (count > 0 && q.vertexBuffer->readCpu(q.vertexOffset, count * stride, data.data())) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (std::uint32_t i = 0; i < count; ++i) {
                float p[3];
                std::memcpy(p, data.data() + static_cast<std::size_t>(i) * stride, 12);
                for (int r = 0; r < 3; ++r) {
                    const float v = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
                    lo[r] = std::min(lo[r], v);
                    hi[r] = std::max(hi[r], v);
                }
            }
            b.vertices = count;
            b.minY = lo[1];
            b.maxY = hi[1];
            b.level = 0.5f * (lo[1] + hi[1]);
            b.extentX = hi[0] - lo[0];
            b.extentZ = hi[2] - lo[2];
            b.sea = std::max(b.extentX, b.extentZ) >= kSeaExtent && (hi[1] - lo[1]) <= kFlatSlack;
            fallCentre_[0] = 0.5f * (lo[0] + hi[0]);
            fallCentre_[1] = 0.5f * (lo[2] + hi[2]);
        }
    }
    const char* name = q.tag->materialName != nullptr ? q.tag->materialName : "?";
    if (!b.sea) {
        for (const char* o : kSeaOverrides) {
            if (*o != '\0' && std::string_view(name).starts_with(o)) b.sea = true;
        }
    }
    spdlog::info("enh: water: body '{}' ({}): {} vertices, extent {:.0f} x {:.0f}, height {:.1f}..{:.1f} -> {}", name,
                 draws::name(q.tag->water), b.vertices, b.extentX, b.extentZ, b.minY, b.maxY,
                 b.sea ? "the open sea (reaches the world's border)" : "inland water");
    return bodies_.emplace(id, b).first->second;
}

// The engine's coast overlay (Special_Water_Shore_*): its triangles in world space, all of the mesh at its first draw
void Water::collectCoast(d3d9::Device& device, const d3d9::ReplaceQuery& q) {
    if (q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.vertexStride < 12) return;
    const std::uint64_t key = q.vertexBuffer->ref().id ^ (q.indexBuffer->ref().id << 1);
    if (!coastMeshes_.insert(key).second) return;
    float rows[3][4];
    if (!worldRows(device, q, rows)) return;
    const std::uint32_t stride = q.vertexStride;
    const std::uint32_t vcount = (q.vertexBuffer->size() - std::min(q.vertexOffset, q.vertexBuffer->size())) / stride;
    std::vector<std::uint8_t> vdata(static_cast<std::size_t>(vcount) * stride);
    const std::uint32_t isize = q.indexBuffer->index32() ? 4u : 2u;
    const std::uint32_t icount = q.indexBuffer->size() / isize;
    std::vector<std::uint8_t> idata(static_cast<std::size_t>(icount) * isize);
    if (vcount == 0 || icount < 3 || !q.vertexBuffer->readCpu(q.vertexOffset, vcount * stride, vdata.data()) ||
        !q.indexBuffer->readCpu(0, icount * isize, idata.data())) {
        return;
    }
    const auto index = [&](std::uint32_t i) -> std::int64_t {
        if (isize == 4) {
            std::uint32_t v;
            std::memcpy(&v, idata.data() + static_cast<std::size_t>(i) * 4, 4);
            return static_cast<std::int64_t>(v) + q.baseVertex;
        }
        std::uint16_t v;
        std::memcpy(&v, idata.data() + static_cast<std::size_t>(i) * 2, 2);
        return static_cast<std::int64_t>(v) + q.baseVertex;
    };
    const auto world = [&](std::int64_t v, float out[3]) {
        float p[3];
        std::memcpy(p, vdata.data() + static_cast<std::size_t>(v) * stride, 12);
        for (int r = 0; r < 3; ++r) out[r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
    };
    std::size_t added = 0, bad = 0;
    for (std::uint32_t i = 0; i + 2 < icount && coast_.size() < (4u << 20); i += 3) {
        const std::int64_t a = index(i), b = index(i + 1), c = index(i + 2);
        if (a < 0 || b < 0 || c < 0 || a >= vcount || b >= vcount || c >= vcount) {
            ++bad;
            continue;
        }
        shore::Triangle t;
        world(a, t.a);
        world(b, t.b);
        world(c, t.c);
        coast_.push_back(t);
        ++added;
    }
    coastDirty_ = added > 0;
    spdlog::info("enh: water: coast overlay '{}': {} triangles ({} indices out of range), {} in the shore field",
                 q.tag->materialName != nullptr ? q.tag->materialName : "?", added, bad, coast_.size());
}

void Water::serviceBake(d3d9::Device& device) {
    if (bake_.valid() && bake_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        shore::Field f = bake_.get();
        if (f.valid()) {
            d3d9::TextureDesc desc;
            desc.width = static_cast<std::uint32_t>(f.width);
            desc.height = static_cast<std::uint32_t>(f.height);
            desc.levels = 1;
            desc.format = vk::d3dfmt::A16B16G16R16F;
            desc.pool = d3d9::POOL_MANAGED;
            auto tex = device.createTexture(desc);
            d3d9::Texture::Locked l;
            if (tex && tex->valid() && tex->lock(0, 0, nullptr, 0, l)) {
                const std::size_t row = static_cast<std::size_t>(f.width) * 8;
                for (int z = 0; z < f.height; ++z) {
                    std::memcpy(static_cast<std::uint8_t*>(l.bits) + static_cast<std::size_t>(z) * l.rowPitch,
                                f.texels.data() + static_cast<std::size_t>(z) * f.width * 4, row);
                }
                tex->unlock(0, 0);
                if (field_) device.release(std::move(field_));
                field_ = std::move(tex);
                fieldMapping_ = f.mapping();
                spdlog::info("enh: water: shore field {}x{} texels of {:.0f} units at ({:.0f}, {:.0f}), {} coast texels", f.width,
                             f.height, f.cell, f.minX, f.minZ, f.coastTexels);
            }
        }
    }
    if (coastDirty_ && !bake_.valid()) {
        coastDirty_ = false;
        bake_ = std::async(std::launch::async, [tris = coast_]() { return shore::bake(tris); });
    }
}

VkDeviceAddress Water::fillBlock(d3d9::Device& device, const d3d9::ReplaceQuery& q, draws::Water kind, bool grid, const Body& b,
                                 std::array<d3d9::Texture*, 6>& sampled) {
    const vk::RingAlloc a = device.frames().allocate(sizeof(WaterBlock), 16);
    if (a.ptr == nullptr) return 0;
    WaterBlock w{};
    const d3d9::Viewport& vp = device.viewport();
    w.posFix[0] = 1.0f / static_cast<float>(std::max(1u, vp.width));
    w.posFix[1] = -1.0f / static_cast<float>(std::max(1u, vp.height));
    copy4(w.sunColor, device.psConstant(22));
    copy4(w.sunSpec, device.psConstant(23));
    copy4(w.sunDir, device.psConstant(24));
    copy4(w.fogColor, device.psConstant(3));
    copy4(w.fogParams, device.psConstant(4));
    for (int i = 0; i < 3; ++i) copy4(w.ambient[i], device.psConstant(15 + static_cast<std::uint32_t>(i)));
    const WaterProfile p = waterProfile(kind, device.psConstant(6), settings_.murkiness, settings_.reflections);
    w.scatter[0] = p.scatter[0];
    w.scatter[1] = p.scatter[1];
    w.scatter[2] = p.scatter[2];
    w.scatter[3] = p.refraction;
    w.absorb[0] = p.absorb[0];
    w.absorb[1] = p.absorb[1];
    w.absorb[2] = p.absorb[2];
    w.absorb[3] = p.reflectivity;
    w.surface[0] = p.roughness;
    w.surface[1] = p.glint;
    w.surface[2] = p.ripple;
    w.surface[3] = p.rippleScale;
    w.foam[0] = p.foam[0];
    w.foam[1] = p.foam[1];
    w.foam[2] = p.foam[2];
    w.foam[3] = p.foamStrength;
    w.look[0] = p.turbidity;
    const int quality = std::clamp(settings_.quality, 0, 2);
    static constexpr int kGridQuads[3] = {48, 64, 96};
    static constexpr std::uint32_t kSsrSteps[3] = {0, 20, 40};
    w.ocean[0] = b.level;
    w.ocean[1] = kWaveHeight * std::max(0.0f, settings_.waveHeight);
    w.ocean[2] = kLowering;
    w.ocean[3] = kSwash;
    w.grid[0] = kGridSpacing;
    w.grid[1] = static_cast<float>(kGridLevels);
    w.grid[2] = static_cast<float>(kGridQuads[quality]);
    w.grid[3] = static_cast<float>(std::fmod(device.secondsSinceCreate(), 16384.0));
    w.shoreMap[0] = fieldMapping_[0];
    w.shoreMap[1] = fieldMapping_[1];
    w.shoreMap[2] = fieldMapping_[2];
    w.shoreMap[3] = fieldMapping_[3];
    w.misc[0] = 1.0f;  // sky cube scale
    w.misc[1] = kNearCap;
    w.misc[2] = kCoastBand;
    w.misc[3] = 30000.0f;  // SSR distance
    // inputs: the engine draw's own refraction copy (s1) and depth map (s2), the sky cube, its shadow mask (s0)
    const shader::Reflection& er = q.enginePs->reflection;
    const auto bound = [&](std::uint32_t slot, shader::TextureType type) -> d3d9::Texture* {
        d3d9::Texture* t = q.textures[slot];
        return er.samplers[slot] == type && t != nullptr && !t->isNull() ? t : nullptr;
    };
    d3d9::Texture* refraction = bound(1, shader::TextureType::Tex2D);
    d3d9::Texture* depth = bound(2, shader::TextureType::Tex2D);
    d3d9::Texture* shadow = bound(0, shader::TextureType::Tex2D);
    d3d9::Texture* sky = device.imageWithRole(d3d9::scene::ImageRole::SkyEnvCube);
    if (sky == nullptr) {
        for (std::uint32_t s = 9; s < 16 && sky == nullptr; ++s) sky = bound(s, shader::TextureType::Cube);
    }
    sampled = {refraction, depth, sky, shadow, field_.get(), nullptr};
    w.tex[0] = refraction != nullptr ? refraction->bindlessSlot() : 0;
    w.tex[1] = depth != nullptr ? depth->bindlessSlot() : 0;
    w.tex[2] = sky != nullptr && sky->kind() == vk::TextureKind::Cube ? sky->bindlessSlot() : 0;
    w.tex[3] = field_ ? field_->bindlessSlot() : 0;
    w.tex2[0] = shadow != nullptr ? shadow->bindlessSlot() : 0;
    w.tex2[1] = settings_.ssr ? kSsrSteps[quality] : 0;
    std::uint32_t flags = 0;
    if (grid) flags |= 1u;  // WATER_OCEAN_GRID
    if (kind == draws::Water::Ocean) flags |= 2u;  // WATER_SEA
    if (q.instanced) flags |= 4u;
    if (field_) flags |= 8u;
    if (grid && device.vsConstant(11)[1] < b.level) flags |= 16u;  // the camera is under the sea's surface
    w.tex2[2] = flags;
    w.tex2[3] = static_cast<std::uint32_t>(std::clamp(settings_.debug, 0, 4));
    w.smp[0] = device.samplerFor(true, true);
    w.smp[1] = device.samplerFor(true, false);
    w.smp[2] = device.samplerFor(false, true);
    // the engine's point lights of this draw (g_PointLights c25 + 4 i: position + 1 / range^2, diffuse, specular, shadow
    // mask), when its pixel shader has them (torches, crystals in caves light the water like everything else)
    {
        const std::uint32_t n = q.enginePs->reflection.floatConstCount > 25 ? (q.enginePs->reflection.floatConstCount - 25) / 4 : 0;
        for (std::uint32_t i = 0; i < std::min(n, 4u); ++i) {
            copy4(w.lights[i * 3 + 0], device.psConstant(25 + i * 4));
            copy4(w.lights[i * 3 + 1], device.psConstant(26 + i * 4));
            copy4(w.lights[i * 3 + 2], device.psConstant(27 + i * 4));
        }
    }
    // the four waterfalls nearest to the camera (whitewater below them)
    {
        const float* cam = device.vsConstant(11);
        std::vector<std::pair<float, const Fall*>> nearest;
        for (const auto& [id, fl] : falls_) {
            const float dx = fl.x - cam[0], dz = fl.z - cam[2];
            nearest.emplace_back(dx * dx + dz * dz, &fl);
        }
        std::sort(nearest.begin(), nearest.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
        for (std::size_t i = 0; i < nearest.size() && i < 4; ++i) {
            const Fall& fl = *nearest[i].second;
            w.falls[i][0] = fl.x;
            w.falls[i][1] = fl.z;
            w.falls[i][2] = fl.radius;
            w.falls[i][3] = fl.baseY;
        }
    }
    std::memcpy(a.ptr, &w, sizeof(w));
    return a.address;
}

void Water::replace(d3d9::Device& device, const d3d9::ReplaceQuery& q, DrawReplacement& out) {
    if (!settings_.enabled || failed_ || q.tag == nullptr) return;
    const draws::Water tagged = q.tag->water;
    serviceBake(device);
    const std::uint64_t frame = device.stats().frames;
    if (tagged == draws::Water::Shore) {
        // the engine's fake shore: its geometry is the coast of the shore field; with the wave grid it is not drawn
        if (settings_.oceanWaves) {
            collectCoast(device, q);
            if (gridFrame_ == frame) {
                out.action = DrawReplacement::Action::Skip;
                out.water = draws::Water::Shore;
                ++skippedDraws_;
            }
        }
        return;
    }
    if (tagged == draws::Water::Waterfall) {
        // the falls keep the engine's shading; their base is where the water below gets whitewater
        if (q.vertexBuffer != nullptr && !falls_.contains(q.vertexBuffer->ref().id)) {
            const Body& fb = body(device, q);
            if (fb.vertices > 0) {
                // the centre of its world box (body() measured it just now), a radius from its width, its lowest point
                const Fall fall{fallCentre_[0], fallCentre_[1], std::max(300.0f, 0.35f * std::max(fb.extentX, fb.extentZ)), fb.minY};
                falls_.emplace(q.vertexBuffer->ref().id, fall);
            }
        }
        return;
    }
    if (q.kind != draws::Kind::Refracted) return;  // other alpha water keeps the engine's shading
    if (tagged != draws::Water::Ocean && tagged != draws::Water::Lake && tagged != draws::Water::River &&
        tagged != draws::Water::Swamp) {
        return;
    }
    const shader::Reflection& er = q.enginePs->reflection;
    if (er.samplers[1] != shader::TextureType::Tex2D || er.samplers[2] != shader::TextureType::Tex2D || q.textures[1] == nullptr ||
        q.textures[2] == nullptr) {
        if (!loggedNoInputs_) {
            loggedNoInputs_ = true;
            spdlog::info("enh: water: a water draw without the refraction copy / depth map (no image effect?): engine water");
        }
        return;
    }
    if (!ensureShaders()) return;
    // until the pipelines are built (in the background) the engine draws its water: no frame waits for a compiler
    if (!pipelinesReady_) {
        pipelinesReady_ = device.replacementReady(*meshVs_, *ps_, false) && device.replacementReady(*oceanVs_, *ps_, true);
        if (!pipelinesReady_) {
            if (!prebuilt_) prebuild();
            return;
        }
        spdlog::info("enh: water: pipelines ready");
    }
    const Body& b = body(device, q);
    const draws::Water kind = b.sea ? draws::Water::Ocean : (tagged == draws::Water::Ocean ? draws::Water::Lake : tagged);
    const bool grid = b.sea && settings_.oceanWaves && !q.instanced;
    if (grid) {
        if (gridFrame_ == frame && gridMainView_ == q.mainView) {
            out.action = DrawReplacement::Action::Skip;  // the grid of this view covers the whole sea
            out.water = kind;
            ++skippedDraws_;
            return;
        }
        gridFrame_ = frame;
        gridMainView_ = q.mainView;
    }
    std::array<d3d9::Texture*, 6> sampled{};
    const VkDeviceAddress block = fillBlock(device, q, kind, grid, b, sampled);
    if (block == 0) return;
    out.action = grid ? DrawReplacement::Action::Procedural : DrawReplacement::Action::Shaders;
    out.vs = grid ? oceanVs_ : meshVs_;
    out.ps = ps_;
    out.block = block;
    out.cullNone = true;  // seen from below too (wading, swimming)
    out.opaque = true;
    out.sampled = sampled;
    out.water = kind;
    if (grid) {
        static constexpr std::uint32_t kGridQuads[3] = {48, 64, 96};
        const std::uint32_t n = kGridQuads[std::clamp(settings_.quality, 0, 2)];
        out.vertices = static_cast<std::uint32_t>(kGridLevels) * n * n * 6;
    }
    ++replacedDraws_;
    if (!loggedFirst_) {
        loggedFirst_ = true;
        spdlog::info("enh: water: first replaced draw: '{}' as {} ({}), refraction slot {}, depth slot {}, sky cube {}, shore field {}",
                     q.tag->materialName != nullptr ? q.tag->materialName : "?", draws::name(kind), grid ? "wave grid" : "its own mesh",
                     sampled[0] != nullptr ? sampled[0]->bindlessSlot() : 0, sampled[1] != nullptr ? sampled[1]->bindlessSlot() : 0,
                     sampled[2] != nullptr ? "yes" : "no", field_ ? "yes" : "not yet");
    }
}

}  // namespace rvk::enh
