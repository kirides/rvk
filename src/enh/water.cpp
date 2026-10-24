#include "enh/water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>

#include <spdlog/spdlog.h>

#include "core/paths.h"
#include "engine/weather.h"
#include "enh/rt_scene.h"
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
constexpr float kLowering = 13.0f;     // the waterline at the coast below the engine's
constexpr float kSwash = 8.0f;         // swash amplitude (< kLowering: the water never climbs above the engine's line)
constexpr float kCoastBand = 500.0f;   // how far from the coast overlay the lowering / swash reach (quays further out keep the level)
constexpr float kWetHeight = 22.0f;     // damp sand above the swash's reach (world units)
constexpr float kWetDarkening = 0.24f;  // wet sand: x (1 - this) of the dry colour
constexpr float kGridSpacing = 25.0f;  // finest grid spacing
constexpr int kGridLevels = 8;

// the FFT ocean (docs/research/ocean.md)
constexpr float kAutoWindAngle = 0.35f;  // rad: the prevailing wind blows from the south-west (towards +x, +z) onto the
                                         // west beaches; water.wind_direction overrides it
constexpr float kSwellOffset = 0.3f;     // the swell comes from a little further south than the wind sea
constexpr float kBreakerIndex = 0.78f;   // waves break at ~0.78 x the water depth
// the sea floor field's grid: fixed for Risen's island (the coast overlay's box -72,600 .. 36,300 x -63,800 .. 46,100 + a
// 30,000-unit margin out to sea), so the disk cache stays valid between sessions and areas
constexpr float kFloorMinX = -104000.0f, kFloorMinZ = -96000.0f, kFloorCell = 84.0f;
constexpr int kFloorSize = 2048;
constexpr std::uint64_t kFloorFrameBudget = 8u << 20;  // bytes of engine geometry read per frame
constexpr std::size_t kFloorMaxPending = 4u << 20;     // triangles waiting for the worker
constexpr double kFloorBakeInterval = 2.0;             // s between bakes while new geometry arrives
constexpr double kFloorSaveInterval = 20.0;            // s between cache writes

seafloor::Grid floorGrid() {
    seafloor::Grid g;
    g.width = g.height = kFloorSize;
    g.minX = kFloorMinX;
    g.minZ = kFloorMinZ;
    g.cell = kFloorCell;
    return g;
}

std::filesystem::path floorCacheFile() { return paths::dataDir() / "cache" / "ocean" / "floor_v1.bin"; }

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::unique_ptr<d3d9::Texture> uploadHalf4(d3d9::Device& device, int width, int height, const std::vector<std::uint16_t>& texels) {
    d3d9::TextureDesc desc;
    desc.width = static_cast<std::uint32_t>(width);
    desc.height = static_cast<std::uint32_t>(height);
    desc.levels = 1;
    desc.format = vk::d3dfmt::A16B16G16R16F;
    desc.pool = d3d9::POOL_MANAGED;
    auto tex = device.createTexture(desc);
    d3d9::Texture::Locked l;
    if (!tex || !tex->valid() || !tex->lock(0, 0, nullptr, 0, l)) return nullptr;
    const std::size_t row = static_cast<std::size_t>(width) * 8;
    for (int z = 0; z < height; ++z) {
        std::memcpy(static_cast<std::uint8_t*>(l.bits) + static_cast<std::size_t>(z) * l.rowPitch,
                    texels.data() + static_cast<std::size_t>(z) * width * 4, row);
    }
    tex->unlock(0, 0);
    return tex;
}

std::uint64_t nameHash(const char* s) {
    std::uint64_t h = 1469598103934665603ull;
    for (; *s != 0; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
    return h;
}

void copy4(float* dst, const float* src) { std::memcpy(dst, src, 16); }
std::uint32_t w0(const d3d9::Texture* t) { return t != nullptr ? t->bindlessSlot() : 0; }

}  // namespace

Water::Water(d3d9::Device& device, RtScene* rt) : dev_(device) {
    // ray-traced reflections (rt.reflections): their compute pipeline is built in the background from the start
    if (rt != nullptr && rt->supported()) rtRefl_ = std::make_unique<WaterRt>(device, rt);
    // the FFT ocean's compute pipelines exist from the start too (its tiles are created when it is first wanted)
    ocean_ = std::make_unique<Ocean>(device);
    floor_ = std::make_shared<FloorState>();
    // the shaders exist from the start, so their pipelines are built in the background at the session's first water draw
    // (also with the feature off): turning it on later costs no frame
    if (ensureShaders()) dev_.registerReplacement(oceanVs_, ps_, true);
    // the wet-beach pass's pipeline for the scene target's format (A16B16G16R16F) now: turning the water on costs no frame
    ensureWetPipeline(VK_FORMAT_R16G16B16A16_SFLOAT);
}

Water::~Water() {
    dev_.removeDrawReplacer(this);
    rtRefl_.reset();
    if (wetPassId_ != 0) dev_.removePass(wetPassId_);
    if (oceanPassId_ != 0) dev_.removePass(oceanPassId_);
    destroyWetPipeline();
    if (bake_.valid()) bake_.wait();
    if (floorBake_.valid()) floorBake_.wait();
    saveFloor();
    if (field_) dev_.release(std::move(field_));
    if (floorA_) dev_.release(std::move(floorA_));
    if (floorB_) dev_.release(std::move(floorB_));
    ocean_.reset();
    destroyShaders();
}

void Water::destroyShaders() {
    vk::Context* ctx = &dev_.ctx();
    for (auto* s : {&oceanVs_, &ps_}) {
        if (*s && (*s)->module != VK_NULL_HANDLE) {
            const VkShaderModule m = (*s)->module;
            dev_.frames().retire([ctx, m] { vkDestroyShaderModule(ctx->device(), m, nullptr); });
        }
        s->reset();
    }
}

bool Water::ensureShaders() {
    if (failed_) return false;
    if (oceanVs_ && ps_) return true;
    const auto make = [&](const char* name, shader::Stage stage) {
        auto s = std::make_shared<d3d9::Shader>();
        s->stage = stage;
        s->module = vk::createShaderModule(dev_.ctx(), shader::embedded::spirv(name));
        s->hash = nameHash(name);
        return s;
    };
    using shader::Usage;
    // the engine's meshes keep the engine's own vertex shader (translated): its interpolators are what water.frag reads
    oceanVs_ = make("water/water_ocean.vert", shader::Stage::Vertex);
    oceanVs_->reflection.floatConstCount = 256;
    ps_ = make("water/water.frag", shader::Stage::Pixel);
    ps_->reflection.floatConstCount = 1;
    ps_->reflection.colorOutputs = 1;
    if (oceanVs_->module == VK_NULL_HANDLE || ps_->module == VK_NULL_HANDLE) {
        spdlog::error("enh: water: shader modules failed, enhanced water stays off this session");
        failed_ = true;
        destroyShaders();
        return false;
    }
    return true;
}

void Water::prebuild() {
    if (prebuilt_ || !ensureShaders()) return;
    prebuilt_ = dev_.prebuildReplacement(*oceanVs_, *ps_, true);
    spdlog::info("enh: water: pipelines {}", prebuilt_ ? "ready or building in the background"
                                                         : "built in the background at the first water draw");
}

void Water::apply(const Settings& s) {
    const bool was = settings_.enabled;
    settings_ = s;
    if (s.enabled) {
        if (!was) {
            dev_.addDrawReplacer(this);
            loggedFirst_ = false;
        }
        prebuild();
    } else if (was) {
        dev_.removeDrawReplacer(this);
    }
    updateWetPass();
    updateRtReflections();
    if (ocean_) {
        if (fftWanted()) ocean_->setQuality(s.oceanQuality);
        else ocean_->release();  // no GPU memory for tiles nobody samples
    }
    updateOceanPass();
    static constexpr const char* kOcean[3] = {"off", "gerstner", "fft"};
    spdlog::info("enh: water: {} (quality {}, reflections {}, ocean {} (quality {}, sea state {}, wind {}), wave scale {:.2f}, murkiness "
                 "{:.2f}, reflection strength {:.2f}, debug {}, ray-traced reflections {})",
                 s.enabled ? "on" : "off", s.quality, s.ssr ? "ssr" : "sky cube", kOcean[std::clamp(s.ocean, 0, 2)], s.oceanQuality,
                 s.seaState, s.windDirection, s.waveHeight, s.murkiness, s.reflections, s.debug,
                 rtWanted() ? std::format("tier {}", s.rtTier)
                            : s.rtTier > 0 ? std::string(rtRefl_ && rtRefl_->available() ? "off (needs water.reflections = ssr)" : "unavailable (no ray tracing tier)")
                                           : std::string("off"));
}

void Water::updateRtReflections() {
    if (!rtRefl_) return;
    rtRefl_->configure(rtWanted() ? settings_.rtTier : 0, settings_.rtTweaks, settings_.debug == 8 ? 1 : settings_.debug == 9 ? 2 : 0);
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
            b.box[0] = lo[0];
            b.box[1] = lo[2];
            b.box[2] = hi[0];
            b.box[3] = hi[2];
            fitPlane(rows, data.data(), count, stride, b);
            measureOutdoor(q, data.data(), count, stride, b);
            fallCentre_[0] = 0.5f * (lo[0] + hi[0]);
            fallCentre_[1] = 0.5f * (lo[2] + hi[2]);
            if (b.sea) {
                measureMapping(q, rows, data.data(), count, b);
                seaLevel_ = b.level;
                seaKnown_ = true;
            }
        }
    }
    const char* name = q.tag->materialName != nullptr ? q.tag->materialName : "?";
    if (!b.sea) {
        for (const char* o : kSeaOverrides) {
            if (*o != '\0' && std::string_view(name).starts_with(o)) b.sea = true;
        }
    }
    spdlog::info("enh: water: body '{}' ({}): {} vertices, centre ({:.0f}, {:.0f}), extent {:.0f} x {:.0f}, height "
                 "{:.1f}..{:.1f} -> {}; plane y = {:.4f} x + {:.4f} z + {:.1f} (max error {:.1f}{}), outdoor {:.2f}",
                 name, draws::name(q.tag->water), b.vertices, fallCentre_[0],
                 fallCentre_[1], b.extentX, b.extentZ, b.minY, b.maxY,
                 b.sea ? "the open sea (reaches the world's border)" : "inland water", b.plane[0], b.plane[1], b.plane[2], b.planeError,
                 b.planeError < 150.0f ? "" : ": no plane, no ray-traced reflections", b.outdoor);
    return bodies_.emplace(id, b).first->second;
}

// The body's surface as a plane y = a x + b z + c (least squares over its world vertices) and the plane's worst error: the
// ray-traced reflections trace the mirror ray of that plane (lakes and ponds are flat, a gently sloping river is a tilted
// plane; a river with falls is none)
void Water::fitPlane(const float rows[3][4], const std::uint8_t* data, std::uint32_t count, std::uint32_t stride, Body& b) {
    if (count < 3) return;
    double sxx = 0, sxz = 0, szz = 0, sx = 0, sz = 0, sy = 0, sxy = 0, szy = 0;
    const double ox = 0.5 * (b.box[0] + b.box[2]), oz = 0.5 * (b.box[1] + b.box[3]);  // centred: a well-conditioned fit
    const auto world = [&](std::uint32_t i, double out[3]) {
        float p[3];
        std::memcpy(p, data + static_cast<std::size_t>(i) * stride, 12);
        for (int r = 0; r < 3; ++r) out[r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        double v[3];
        world(i, v);
        const double x = v[0] - ox, z = v[2] - oz, y = v[1];
        sxx += x * x;
        sxz += x * z;
        szz += z * z;
        sx += x;
        sz += z;
        sy += y;
        sxy += x * y;
        szy += z * y;
    }
    // normal equations [sxx sxz sx; sxz szz sz; sx sz n] [a b c]' = [sxy szy sy]'
    const double n = count;
    const double m[3][3] = {{sxx, sxz, sx}, {sxz, szz, sz}, {sx, sz, n}};
    const double r[3] = {sxy, szy, sy};
    const auto det3 = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double d = det3(m);
    double coef[3];
    if (std::abs(d) < 1e-9) {
        coef[0] = coef[1] = 0.0;  // degenerate (a line of vertices): flat at the mean height
        coef[2] = sy / n;
    } else {
        for (int k = 0; k < 3; ++k) {
            double mk[3][3];
            std::memcpy(mk, m, sizeof(m));
            for (int row = 0; row < 3; ++row) mk[row][k] = r[row];
            coef[k] = det3(mk) / d;
        }
    }
    double err = 0.0;
    for (std::uint32_t i = 0; i < count; ++i) {
        double v[3];
        world(i, v);
        err = std::max(err, std::abs(coef[0] * (v[0] - ox) + coef[1] * (v[2] - oz) + coef[2] - v[1]));
    }
    b.plane[0] = static_cast<float>(coef[0]);
    b.plane[1] = static_cast<float>(coef[1]);
    b.plane[2] = static_cast<float>(coef[2] - coef[0] * ox - coef[1] * oz);
    b.planeError = static_cast<float>(err);
}

// the body's outdoor factor (COLOR1.w of its vertices, the engine water VS's indoor / outdoor blend), averaged
void Water::measureOutdoor(const d3d9::ReplaceQuery& q, const std::uint8_t* data, std::uint32_t count, std::uint32_t stride, Body& b) {
    if (q.decl == nullptr || count == 0) return;
    for (const d3d9::VertexElement& e : q.decl->elements()) {
        if (e.stream != 0 || e.usage != 10 || e.usageIndex != 1 || e.type != 4) continue;  // COLOR1 D3DCOLOR
        double sum = 0.0;
        for (std::uint32_t i = 0; i < count; ++i) sum += data[static_cast<std::size_t>(i) * stride + e.offset + 3];
        b.outdoor = static_cast<float>(sum / count / 255.0);
        return;
    }
}

// The sea mesh's texture mapping (texcoord0 as an affine function of world x, z), tangent frame and outdoor factor: the
// wave grid hands water.frag the same interpolators as the engine's ocean VS would for the plane
void Water::measureMapping(const d3d9::ReplaceQuery& q, const float rows[3][4], const std::uint8_t* data, std::uint32_t count, Body& b) {
    if (q.decl == nullptr) return;
    const d3d9::VertexElement* pos = nullptr;
    const d3d9::VertexElement* uv = nullptr;
    const d3d9::VertexElement* tan = nullptr;
    const d3d9::VertexElement* col0 = nullptr;
    const d3d9::VertexElement* col1 = nullptr;
    for (const d3d9::VertexElement& e : q.decl->elements()) {
        if (e.stream != 0) continue;
        if (e.usage == 0 && e.usageIndex == 0 && e.type == 2) pos = &e;    // POSITION FLOAT3
        if (e.usage == 5 && e.usageIndex == 0 && e.type == 1) uv = &e;     // TEXCOORD0 FLOAT2
        if (e.usage == 6 && e.usageIndex == 0 && e.type == 2) tan = &e;    // TANGENT FLOAT3
        if (e.usage == 10 && e.usageIndex == 0 && e.type == 4) col0 = &e;  // COLOR0 D3DCOLOR
        if (e.usage == 10 && e.usageIndex == 1 && e.type == 4) col1 = &e;  // COLOR1 D3DCOLOR
    }
    const std::uint32_t stride = q.vertexStride;
    if (pos == nullptr || uv == nullptr || count < 3) return;
    std::vector<float> xs(count), zs(count), us(count), vs(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* v = data + static_cast<std::size_t>(i) * stride;
        float p[3], t[2];
        std::memcpy(p, v + pos->offset, 12);
        std::memcpy(t, v + uv->offset, 8);
        xs[i] = rows[0][0] * p[0] + rows[0][1] * p[1] + rows[0][2] * p[2] + rows[0][3];
        zs[i] = rows[2][0] * p[0] + rows[2][1] * p[1] + rows[2][2] * p[2] + rows[2][3];
        us[i] = t[0];
        vs[i] = t[1];
    }
    float eu = 0.0f, ev = 0.0f;
    b.mapped = fitAffineXZ(xs.data(), zs.data(), us.data(), count, b.uvU, &eu) &&
               fitAffineXZ(xs.data(), zs.data(), vs.data(), count, b.uvV, &ev);
    if (tan != nullptr) {
        float tv[3];
        std::memcpy(tv, data + tan->offset, 12);
        float wt[3];
        for (int r = 0; r < 3; ++r) wt[r] = rows[r][0] * tv[0] + rows[r][1] * tv[1] + rows[r][2] * tv[2];
        const float len = std::sqrt(wt[0] * wt[0] + wt[1] * wt[1] + wt[2] * wt[2]);
        if (len > 1e-6f) {
            for (int r = 0; r < 3; ++r) b.tangent[r] = wt[r] / len;
        }
    }
    // D3DCOLOR is B, G, R, A in memory: the engine VS's binormal sign is 1 - 2 * COLOR0.x (red), the outdoor factor COLOR1.w
    if (col0 != nullptr) b.binormalSign = 1.0f - 2.0f * static_cast<float>(data[col0->offset + 2]) / 255.0f;
    if (col1 != nullptr) b.outdoor = static_cast<float>(data[col1->offset + 3]) / 255.0f;
    spdlog::info("enh: water: sea mesh mapping {}: u = {:.6g} x + {:.6g} z + {:.6g} (max error {:.3g}), v = {:.6g} x + {:.6g} z + "
                 "{:.6g} (max error {:.3g}), tangent ({:.2f}, {:.2f}, {:.2f}), binormal sign {:.2f}, outdoor {:.2f}",
                 b.mapped ? "fitted" : "failed", b.uvU[0], b.uvU[1], b.uvU[2], eu, b.uvV[0], b.uvV[1], b.uvV[2], ev, b.tangent[0],
                 b.tangent[1], b.tangent[2], b.binormalSign, b.outdoor);
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
            overlayField_ = f;  // the floor field records the distance to the overlay (beaches) too
            floorDirty_ = true;
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

VkDeviceAddress Water::fillBlock(d3d9::Device& device, const d3d9::ReplaceQuery& q, draws::Water kind, WaterModel model, bool grid,
                                 const Body& b, std::array<d3d9::Texture*, 16>& sampled) {
    const vk::RingAlloc a = device.frames().allocate(sizeof(WaterBlock), 16);
    if (a.ptr == nullptr) return 0;
    WaterBlock w{};
    // the DrawBlock part: the engine draw's textures and samplers by slot (water.frag samples the material through them)
    const shader::Reflection& er = q.enginePs->reflection;
    sampled = {};
    for (std::uint32_t slot = 0; slot < 16; ++slot) {
        if (er.samplers[slot] == shader::TextureType::Unknown) continue;
        d3d9::Texture* t = q.textures[slot];
        if (t == nullptr || t->isNull()) continue;
        const bool cube = t->kind() == vk::TextureKind::Cube;
        if (cube != (er.samplers[slot] == shader::TextureType::Cube)) continue;  // a mismatching bind: the default texture
        w.tex[slot] = t->bindlessSlot();
        w.smp[slot] = device.samplerOfSlot(slot);
        sampled[slot] = t;
    }
    const d3d9::Viewport& vp = device.viewport();
    w.posFix[0] = 1.0f / static_cast<float>(std::max(1u, vp.width));
    w.posFix[1] = -1.0f / static_cast<float>(std::max(1u, vp.height));
    {
        float jx, jy;
        device.geometryJitter(jx, jy);
        w.posFix[0] += jx;
        w.posFix[1] += jy;
    }
    copy4(w.camFar, device.psConstant(0));
    copy4(w.fogColor, device.psConstant(3));
    copy4(w.fogParams, device.psConstant(4));
    for (std::uint32_t i = 0; i < 8; ++i) copy4(w.mat[i], device.psConstant(6 + i));
    copy4(w.indoor, device.psConstant(14));
    for (std::uint32_t i = 0; i < 7; ++i) copy4(w.sh[i], device.psConstant(15 + i));
    copy4(w.sunColor, device.psConstant(22));
    copy4(w.sunSpec, device.psConstant(23));
    copy4(w.sunDir, device.psConstant(24));
    for (std::uint32_t i = 0; i < 6; ++i) copy4(w.vsTc[i], device.vsConstant(232 + i));
    for (int i = 0; i < 3; ++i) {
        w.uvMapU[i] = b.uvU[i];
        w.uvMapV[i] = b.uvV[i];
        w.meshFrame[i] = b.tangent[i];
    }
    w.meshFrame[3] = b.binormalSign;
    w.meshExtra[0] = b.outdoor;
    w.meshExtra[1] = device.vsConstant(32)[3];  // the global alpha the engine VS passes on (c32.w)
    const WaterProfile p = waterProfile(kind, settings_.murkiness, settings_.reflections, settings_.waveHeight);
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
    w.grid[3] = static_cast<float>(std::fmod(seaClock(), 16384.0));
    for (int i = 0; i < 4; ++i) w.shoreMap[i] = fieldMapping_[static_cast<std::size_t>(i)];
    w.look[0] = p.reflection;
    w.look[1] = p.foam;
    w.look[2] = p.extraReflection;
    w.look[3] = p.shoreWaves;
    w.misc[0] = kNearCap;
    w.misc[1] = kCoastBand;
    w.misc[2] = 30000.0f;  // SSR distance
    w.misc[3] = p.murkiness;
    // the sky cube (the swamp's dim reflection: its shader binds none) and the shore field
    d3d9::Texture* sky = device.imageWithRole(d3d9::scene::ImageRole::SkyEnvCube);
    if (sky != nullptr && !sky->isNull() && sky->kind() == vk::TextureKind::Cube) {
        w.res[0] = sky->bindlessSlot();
        sampled[4] = sky;
    }
    if (field_) {
        w.res[1] = field_->bindlessSlot();
        sampled[3] = field_.get();
    }
    std::uint32_t flags = 0;
    if (grid) flags |= 1u;                                         // WATER_OCEAN_GRID
    if (kind == draws::Water::Ocean) flags |= 2u;                  // WATER_SEA
    if (q.blended && !grid) flags |= 4u;                           // WATER_BLENDED
    if (grid && device.vsConstant(11)[1] < b.level) flags |= 16u;  // the camera is under the sea's surface
    w.res[2] = flags;
    w.res[3] = static_cast<std::uint32_t>(std::clamp(settings_.debug, 0, 9));
    w.res2[0] = static_cast<std::uint32_t>(model);
    w.res2[1] = settings_.ssr ? kSsrSteps[quality] : 0;
    w.res2[2] = w.tex[1];  // the refraction copy (s1 of every water shader)
    w.res2[3] = w.tex[2];  // the depth map (s2)
    w.smpOurs[0] = device.samplerFor(true, true);
    w.smpOurs[1] = device.samplerFor(true, false);
    w.smpOurs[2] = device.samplerFor(false, true);
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
    // the engine's point lights of this draw (g_PointLights c25 + 4 i: position + 1 / range^2, diffuse colour, -, shadow-mask
    // channel), when its pixel shader has them (the cave lake: crystals light the water as everything else)
    if (er.floatConstCount > 25) {
        const std::uint32_t n = std::min<std::uint32_t>((er.floatConstCount - 25) / 4, 4u);
        for (std::uint32_t i = 0; i < n; ++i) {
            copy4(w.lights[i * 3 + 0], device.psConstant(25 + i * 4));
            copy4(w.lights[i * 3 + 1], device.psConstant(26 + i * 4));
            copy4(w.lights[i * 3 + 2], device.psConstant(28 + i * 4));
        }
        w.lightInfo[0] = n;
    }
    // the FFT ocean (its tiles were simulated at AfterOpaque this frame) and the sea floor field
    if (grid && fftWanted() && ocean_ && ocean_->ready()) {
        const ocean::Cascades& cs = ocean_->cascades();
        const float peak = ocean_->peakWavelength();
        for (int c = 0; c < 4; ++c) {
            if (c >= cs.count) continue;
            const ocean::Cascade& cc = cs.c[static_cast<std::size_t>(c)];
            w.fftLength[c] = cc.length * ocean::kUnitsPerMetre;
            w.fftDisp[c] = ocean_->dispSlot(c);
            w.fftDeriv[c] = ocean_->derivSlot(c);
            w.fftSlopeVar[c] = cc.slopeVar;
            // the band's typical wave length: the wind sea's peak when it lies inside the band, else the nearest edge
            const float shortest = 2.0f * static_cast<float>(ocean::kPi) / std::min(cc.kHigh, 1e6f);
            const float longest = cc.kLow > 0.0f ? 2.0f * static_cast<float>(ocean::kPi) / cc.kLow : cc.length;
            w.fftBand[c] = std::clamp(peak, shortest, longest) * ocean::kUnitsPerMetre;
        }
        w.fftInfo[0] = 1;
        w.fftInfo[1] = static_cast<std::uint32_t>(cs.count);
        w.fftInfo[2] = ocean_->sampler();
        w.fftInfo[3] = static_cast<std::uint32_t>(cs.size);
        w.seaInfo[0] = ocean_->significantHeight() * ocean::kUnitsPerMetre;
        w.seaInfo[1] = peak * ocean::kUnitsPerMetre;
        w.seaInfo[2] = ocean_->sea().windAngle;
        w.seaInfo[3] = seaLevelState_;
        const std::array<float, 3> sw = shoreWave();
        for (int i = 0; i < 3; ++i) w.shoreWave[i] = sw[static_cast<std::size_t>(i)];
        w.misc[0] = kNearCap;
        for (int i = 0; i < 4; ++i) w.player[i] = player_[i];
        if (ocean_->foamReady()) {
            const std::array<float, 4> fm = ocean_->foamMap();
            for (int i = 0; i < 4; ++i) w.foamMap[i] = fm[static_cast<std::size_t>(i)];
            w.foamTex[0] = ocean_->foamSlot();
            w.foamTex[1] = ocean_->sampler();
        }
        if (floorA_ && floorB_) {
            for (int i = 0; i < 4; ++i) w.floorMap[i] = floorMapping_[static_cast<std::size_t>(i)];
            w.floorTex[0] = floorA_->bindlessSlot();
            w.floorTex[1] = floorB_->bindlessSlot();
            w.floorTex[2] = 1;
            sampled[5] = floorA_.get();
            sampled[6] = floorB_.get();
        }
    }
    // ray-traced reflections (enh/water_rt.h): this frame's reflection image, and the light its hits are shaded with
    if (rtRefl_ && rtRefl_->tier() > 0) {
        w.rtRefl[0] = rtRefl_->imageSlot(device.frameValue(), q.mainView);
        w.rtRefl[1] = rtRefl_->flags();
        WaterRt::Light l;
        std::memcpy(l.fogColor, w.fogColor, 16);
        std::memcpy(l.fogParams, w.fogParams, 16);
        std::memcpy(l.indoor, w.indoor, 16);
        std::memcpy(l.sh, w.sh, sizeof(l.sh));
        std::memcpy(l.sunColor, w.sunColor, 16);
        std::memcpy(l.sunDir, w.sunDir, 16);
        rtRefl_->setLight(l);
    }
    std::memcpy(a.ptr, &w, sizeof(w));
    return a.address;
}

// ---- the FFT ocean: simulation pass, sea state ------------------------------------------------------------------------------

void Water::updateOceanPass() {
    const bool want = fftWanted() && ocean_ && !ocean_->failed();
    if (want && oceanPassId_ == 0) {
        oceanPassId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "water.ocean", [this](d3d9::PassContext& ctx) { recordOcean(ctx); },
                                    this, -10);
    } else if (!want && oceanPassId_ != 0) {
        dev_.removePass(oceanPassId_);
        oceanPassId_ = 0;
    }
}

std::array<float, 3> Water::shoreWave() const {
    // the waves rolling in: their period grows with the sea, their height follows Hs (the floor field attenuates them)
    const float hs = ocean_ ? ocean_->significantHeight() * ocean::kUnitsPerMetre : 0.0f;
    return {6.0f + 1.5f * seaLevelState_, std::clamp(0.45f * hs, 25.0f, 160.0f) * std::min(std::max(settings_.waveHeight, 0.0f), 1.0f),
            kBreakerIndex};
}

void Water::updateSea(double seconds) {
    // after a jump of the water's clock the sea takes the weather's state at once (a pinned capture sees the same sea
    // whatever the spot before it was)
    const bool snap = clockJumped_;
    clockJumped_ = false;
    if (snap) lastWeatherRead_ = -1e18;
    float target = 1.0f;
    if (settings_.seaState >= 0) {
        target = static_cast<float>(settings_.seaState);
    } else {
        if (seconds - lastWeatherRead_ > 0.5) {
            lastWeatherRead_ = seconds;
            const std::vector<engine::WeatherState> states = engine::weatherStates();
            if (const std::optional<float> lvl = engine::seaLevelOfWeather(states)) {
                if (!haveWeather_ || std::abs(*lvl - weatherLevel_) > 0.2f || !loggedWeather_) {
                    std::string list;
                    for (const auto& st : states) {
                        if (st.weight > 1e-3f) list += std::format("{} {:.2f}, ", st.name, st.weight);
                    }
                    spdlog::info("enh: ocean: weather {}-> sea state {:.2f}", list, *lvl);
                    loggedWeather_ = true;
                }
                weatherLevel_ = *lvl;
                haveWeather_ = true;
            }
        }
        target = haveWeather_ ? weatherLevel_ : 1.0f;
    }
    const double dt = lastSeaSeconds_ < 0.0 ? 0.0 : std::clamp(seconds - lastSeaSeconds_, 0.0, 1.0);
    lastSeaSeconds_ = seconds;
    if (!haveSeaLevel_ || snap) {
        seaLevelState_ = target;
        haveSeaLevel_ = true;
    } else {
        // the sea follows the weather over a few seconds (waves grow as the storm comes)
        seaLevelState_ += (target - seaLevelState_) * static_cast<float>(1.0 - std::exp(-dt / 3.0));
    }
    const float level = std::round(seaLevelState_ * 20.0f) / 20.0f;  // the spectrum is re-evaluated in steps
    ocean::SeaState s = ocean::seaStateForLevel(level);
    s.windAngle = settings_.windDirection >= 0 ? static_cast<float>(settings_.windDirection) * static_cast<float>(ocean::kPi) / 180.0f
                                               : kAutoWindAngle;
    s.swellAngle = s.windAngle + kSwellOffset;
    s.scale = std::max(settings_.waveHeight, 0.0f);
    ocean_->setSea(s);
}

double Water::seaClock() {
    const std::uint64_t frame = dev_.stats().frames;
    if (frame == clockFrame_ && clock_ >= 0.0) return clock_;
    clockFrame_ = frame;
    const double real = dev_.secondsSinceCreate();
    const double realGap = clockLastReal_ < 0.0 ? 1e9 : real - clockLastReal_;
    const double realDt = std::clamp(realGap, 0.0, 0.25);
    clockLastReal_ = real;
    const std::optional<std::uint32_t> ms = engine::gameTimeMs();
    if (!ms) {
        clock_ = clock_ < 0.0 ? real : clock_ + realDt;
        return clock_;
    }
    const double game = static_cast<double>(*ms) / 1000.0;
    if (clock_ < 0.0 || clockLastGame_ < 0.0 || realGap > 0.5) {
        clock_ = game;  // the first frame with water, or the first after a while without
        clockJumped_ = true;
    } else {
        const double d = game - clockLastGame_;
        if (d > 0.0 && d < 1.0) {
            clock_ += d;
        } else if (d == 0.0) {
            // the game time stands still: the time freeze (deterministic steps: a capture under it sees the same water in
            // every run), a loading screen or a menu (real time)
            const std::optional<std::uint32_t> st = engine::appState();
            const bool frozen = st && *st == 8u;
            clock_ += frozen ? 1.0 / 60.0 : realDt;
            if (frozen != clockFrozen_) {
                clockFrozen_ = frozen;
                if (clockFreezes_++ < 8) spdlog::info("enh: water: clock {} (app state {})", frozen ? "in fixed steps under the time freeze" : "on real time", st ? *st : 0u);
            }
        } else {
            clock_ = game;  // the game time jumped (set by a tool, a loaded save): the water follows
            clockJumped_ = true;
            if (clockJumps_++ < 8) spdlog::info("enh: water: clock follows a game time jump of {:.1f} s to {:.3f} s", d, game);
        }
    }
    clockLastGame_ = game;
    if (!loggedClock_) {
        spdlog::info("enh: water: clock from the engine's game time ({:.3f} s)", game);
        loggedClock_ = true;
    }
    return clock_;
}

void Water::recordOcean(d3d9::PassContext& ctx) {
    if (ctx.mainView != 1 || !fftWanted() || !ocean_ || ocean_->failed()) return;
    serviceFloor(dev_);
    const std::uint64_t frame = dev_.stats().frames;
    // only while the sea is seen (it was drawn lately, or it is known and the tiles are not filled yet)
    if (!seaKnown_ || (ocean_->ready() && (lastGridFrame_ == 0 || frame > lastGridFrame_ + 30))) return;
    const double seconds = seaClock();
    if (clockJumped_) ocean_->resetHistory();
    updateSea(seconds);
    ocean_->record(ctx.cmd, ctx.frameBlock, seconds);
    lastSimFrame_ = frame;
    // the player (the near-water wave cap follows him)
    player_[3] = 0.0f;
    if (const std::optional<std::array<float, 3>> pl = engine::playerPosition()) {
        for (int i = 0; i < 3; ++i) player_[i] = (*pl)[static_cast<std::size_t>(i)];
        player_[3] = 1.0f;
        if (!loggedPlayer_) {
            const float* cam = dev_.vsConstant(11);
            spdlog::info("enh: ocean: player position {:.0f} {:.0f} {:.0f} (camera {:.0f} {:.0f} {:.0f})", player_[0], player_[1], player_[2],
                         cam[0], cam[1], cam[2]);
            loggedPlayer_ = true;
        }
    }
    // the surf's foam around the camera (needs the floor field)
    if (floorA_ && floorB_ && dev_.readable(floorA_.get(), ctx.cmd) && dev_.readable(floorB_.get(), ctx.cmd)) {
        OceanFoamInputs in;
        const float* cam = dev_.vsConstant(11);
        in.camX = cam[0];
        in.camZ = cam[2];
        for (int i = 0; i < 4; ++i) in.floorMap[i] = floorMapping_[static_cast<std::size_t>(i)];
        in.floorA = floorA_->bindlessSlot();
        in.floorB = floorB_->bindlessSlot();
        in.smpClamp = dev_.samplerFor(true, true);
        const std::array<float, 3> sw = shoreWave();
        for (int i = 0; i < 3; ++i) in.shoreWave[i] = sw[static_cast<std::size_t>(i)];
        in.shoreWave[3] = static_cast<float>(std::fmod(seconds, 16384.0));
        in.seconds = seconds;
        ocean_->recordFoam(ctx.cmd, ctx.frameBlock, in);
    }
}

// ---- the sea floor field ----------------------------------------------------------------------------------------------------

bool Water::wantsOpaque() const { return fftWanted() && settings_.oceanWaves && floorPending_.size() < kFloorMaxPending; }

void Water::observeOpaque(d3d9::Device& device, const d3d9::ReplaceQuery& q) {
    if (q.vertexBuffer == nullptr || q.indexBuffer == nullptr || q.decl == nullptr || q.instanced || q.vertexStride < 12 ||
        q.primitives == 0 || (q.primitiveType != 4 && q.primitiveType != 5)) {
        return;
    }
    const d3d9::VertexElement* pos = nullptr;
    for (const d3d9::VertexElement& e : q.decl->elements()) {
        if (e.stream != 0) continue;
        if (e.usage == 2) return;                        // BLENDINDICES: skinned
        if (e.usage == 5 && e.usageIndex == 4) return;  // TEXCOORD4: the engine's instance index (constant instancing)
        if (e.usage == 0 && e.usageIndex == 0 && e.type == 2) pos = &e;
    }
    if (pos == nullptr) return;
    float rows[3][4];
    if (!worldRows(device, q, rows)) return;
    // one entry per mesh range and placement
    std::uint64_t key = 1469598103934665603ull;
    const auto mix = [&key](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) key = (key ^ b[i]) * 1099511628211ull;
    };
    const std::uint64_t ids[2] = {q.vertexBuffer->ref().id, q.indexBuffer->ref().id};
    const std::uint32_t range[4] = {q.first, q.primitives, static_cast<std::uint32_t>(q.baseVertex), q.vertexOffset};
    mix(ids, sizeof(ids));
    mix(range, sizeof(range));
    mix(rows, sizeof(rows));
    if (floorMeshes_.contains(key)) return;
    const std::uint64_t frame = device.stats().frames;
    if (frame != floorFrame_) {
        floorFrame_ = frame;
        floorFrameBytes_ = 0;
    }
    if (floorFrameBytes_ > kFloorFrameBudget) return;  // next frame (it is drawn again)
    const std::uint32_t isize = q.indexBuffer->index32() ? 4u : 2u;
    const std::uint32_t icount = q.primitiveType == 4 ? q.primitives * 3 : q.primitives + 2;
    std::vector<std::uint8_t> idata(static_cast<std::size_t>(icount) * isize);
    if (!q.indexBuffer->readCpu(q.first * isize, icount * isize, idata.data())) return;
    std::vector<std::uint32_t> idx(icount);
    std::uint32_t lo = 0xffffffffu, hi = 0;
    for (std::uint32_t i = 0; i < icount; ++i) {
        std::uint32_t v = 0;
        if (isize == 4) std::memcpy(&v, idata.data() + static_cast<std::size_t>(i) * 4, 4);
        else {
            std::uint16_t v16 = 0;
            std::memcpy(&v16, idata.data() + static_cast<std::size_t>(i) * 2, 2);
            v = v16;
        }
        const std::int64_t vv = static_cast<std::int64_t>(v) + q.baseVertex;
        if (vv < 0) return;
        idx[i] = static_cast<std::uint32_t>(vv);
        lo = std::min(lo, idx[i]);
        hi = std::max(hi, idx[i]);
    }
    const std::uint32_t stride = q.vertexStride;
    const std::uint64_t vbytes = static_cast<std::uint64_t>(hi - lo + 1) * stride;
    if (vbytes > (64u << 20)) return;
    std::vector<std::uint8_t> vdata(static_cast<std::size_t>(vbytes));
    if (!q.vertexBuffer->readCpu(q.vertexOffset + lo * stride, static_cast<std::uint32_t>(vbytes), vdata.data())) return;
    floorMeshes_.insert(key);
    floorFrameBytes_ += vbytes + idata.size();
    const seafloor::Grid g = floorGrid();
    const float gx1 = g.minX + static_cast<float>(g.width) * g.cell, gz1 = g.minZ + static_cast<float>(g.height) * g.cell;
    const auto world = [&](std::uint32_t v, float out[3]) {
        float p[3];
        std::memcpy(p, vdata.data() + static_cast<std::size_t>(v - lo) * stride + pos->offset, 12);
        for (int r = 0; r < 3; ++r) out[r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
    };
    std::size_t added = 0;
    const std::uint32_t tris = q.primitiveType == 4 ? icount / 3 : icount - 2;
    for (std::uint32_t t = 0; t < tris; ++t) {
        std::uint32_t a, b, c;
        if (q.primitiveType == 4) {
            a = idx[t * 3];
            b = idx[t * 3 + 1];
            c = idx[t * 3 + 2];
        } else {
            a = idx[t];
            b = idx[t + 1];
            c = idx[t + 2];
            if (a == b || b == c || a == c) continue;  // strip restarts (degenerate)
        }
        shore::Triangle tri;
        world(a, tri.a);
        world(b, tri.b);
        world(c, tri.c);
        const float minX = std::min({tri.a[0], tri.b[0], tri.c[0]}), maxX = std::max({tri.a[0], tri.b[0], tri.c[0]});
        const float minZ = std::min({tri.a[2], tri.b[2], tri.c[2]}), maxZ = std::max({tri.a[2], tri.b[2], tri.c[2]});
        if (maxX < g.minX || maxZ < g.minZ || minX > gx1 || minZ > gz1) continue;
        floorPending_.push_back(tri);
        ++added;
    }
    ++floorMeshCount_;
    if (added > 0) floorDirty_ = true;
}

void Water::serviceFloor(d3d9::Device& device) {
    if (floorBake_.valid() && floorBake_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        seafloor::Field f = floorBake_.get();
        if (f.valid()) {
            auto a = uploadHalf4(device, f.grid.width, f.grid.height, f.a);
            auto b = uploadHalf4(device, f.grid.width, f.grid.height, f.b);
            if (a && b) {
                if (floorA_) device.release(std::move(floorA_));
                if (floorB_) device.release(std::move(floorB_));
                floorA_ = std::move(a);
                floorB_ = std::move(b);
                floorMapping_ = f.grid.mapping();
            }
            spdlog::info("enh: ocean: sea floor field {}x{} of {:.0f} units: {} texels seen ({} land, {} water), max travel time {:.1f} s, "
                         "{} meshes observed, {:.0f} ms (bake: distance {:.0f}, travel time {:.0f}, exposure {:.0f}, pack {:.0f} ms)",
                         f.grid.width, f.grid.height, f.grid.cell, f.knownTexels, f.landTexels, f.waterTexels, f.maxTravel, floorMeshCount_,
                         (steadySeconds() - lastFloorBake_) * 1000.0, f.msDistance, f.msTravel, f.msExposure, f.msPack);
        }
    }
    const double now = steadySeconds();
    if (floorBake_.valid() || !seaKnown_ || !floorDirty_ || now - lastFloorBake_ < kFloorBakeInterval) return;
    floorDirty_ = false;
    lastFloorBake_ = now;
    std::vector<shore::Triangle> tris;
    tris.swap(floorPending_);
    floorBake_ = std::async(std::launch::async, [st = floor_, tris = std::move(tris), level = seaLevel_, overlay = overlayField_]() {
        if (!st->loaded) {
            st->heights.reset(floorGrid());
            const bool cached = st->heights.load(floorCacheFile());
            st->loaded = true;
            st->lastSave = steadySeconds();
            spdlog::info("enh: ocean: sea floor cache {}: {}", floorCacheFile().string(),
                         cached ? std::format("{} texels known", st->heights.known()) : std::string("none yet"));
        }
        st->heights.add(tris);
        st->unsaved += tris.size();
        seafloor::Field f = seafloor::bake(st->heights, level, overlay);
        if (st->unsaved > 0 && steadySeconds() - st->lastSave > kFloorSaveInterval) {
            if (st->heights.save(floorCacheFile())) st->unsaved = 0;
            st->lastSave = steadySeconds();
        }
        return f;
    });
}

void Water::saveFloor() {
    if (floor_ && floor_->loaded && floor_->unsaved > 0) {
        if (floor_->heights.save(floorCacheFile())) {
            spdlog::info("enh: ocean: sea floor cache saved ({} texels known)", floor_->heights.known());
            floor_->unsaved = 0;
        }
    }
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
    // the engine's shader of this material: one of the known water models, else the engine draws it
    const WaterModel model = engineWaterModel(er.samplers.data());
    if (loggedModels_.insert(q.enginePs->hash).second) {
        spdlog::info("enh: water: material '{}' ({}): pixel shader {:016x} -> model {}{}",
                     q.tag->materialName != nullptr ? q.tag->materialName : "?", draws::name(tagged), q.enginePs->hash, name(model),
                     model == WaterModel::None ? " (not a known water shader: the engine's water)" : "");
    }
    if (model == WaterModel::None || !ensureShaders()) return;
    const Body& b = body(device, q);
    const draws::Water kind = b.sea ? draws::Water::Ocean : (tagged == draws::Water::Ocean ? draws::Water::Lake : tagged);
    const bool grid = b.sea && settings_.oceanWaves && !q.instanced && model == WaterModel::Ocean && b.mapped;
    // until the pipelines are built (in the background) the engine draws its water: no frame waits for a compiler
    if (grid) {
        if (!pipelinesReady_) {
            pipelinesReady_ = device.replacementReady(*oceanVs_, *ps_, true);
            if (!pipelinesReady_) {
                if (!prebuilt_) prebuild();
                return;
            }
            spdlog::info("enh: water: wave grid pipeline ready");
        }
        if (gridFrame_ == frame && gridMainView_ == q.mainView) {
            out.action = DrawReplacement::Action::Skip;  // the grid of this view covers the whole sea
            out.water = kind;
            ++skippedDraws_;
            return;
        }
    } else {
        // the engine's mesh with the engine's own vertex shader: one background pipeline build per engine VS
        if (q.engineVs == nullptr) return;
        if (meshVsRegistered_.insert(q.engineVs->hash).second) {
            if (std::shared_ptr<d3d9::Shader> owner = device.shaderOwner(q.engineVs)) device.registerReplacement(std::move(owner), ps_, false);
        }
        if (!device.replacementReady(*q.engineVs, *ps_, false)) return;
    }
    std::array<d3d9::Texture*, 16> sampled{};
    const VkDeviceAddress block = fillBlock(device, q, kind, model, grid, b, sampled);
    if (block == 0) return;
    if (grid) {
        gridFrame_ = frame;
        gridMainView_ = q.mainView;
        lastGridFrame_ = frame;
        seaLevel_ = b.level;
    }
    out.action = grid ? DrawReplacement::Action::Procedural : DrawReplacement::Action::Shaders;
    out.vs = grid ? oceanVs_ : device.shaderOwner(q.engineVs);
    if (!out.vs) return;
    out.ps = ps_;
    // the ray-traced reflections' surfaces: this body as a plane (the next frame's pass; not for bodies that are no plane)
    if (rtRefl_ && rtRefl_->tier() > 0 && q.mainView == 1 && b.planeError < 150.0f && q.vertexBuffer != nullptr) {
        WaterRt::Body rb;
        std::memcpy(rb.plane, b.plane, sizeof(rb.plane));
        std::memcpy(rb.box, b.box, sizeof(rb.box));
        rb.outdoor = b.outdoor;
        rtRefl_->seeBody(q.vertexBuffer->ref().id, rb, device.frameValue());
    }
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
        spdlog::info("enh: water: first replaced draw: '{}' as {} ({}, model {}), refraction slot {}, depth slot {}, shore field {}",
                     q.tag->materialName != nullptr ? q.tag->materialName : "?", draws::name(kind), grid ? "wave grid" : "its own mesh",
                     name(model), w0(sampled[1]), w0(sampled[2]), field_ ? "yes" : "not yet");
    }
}

// ---- the wet beach --------------------------------------------------------------------------------------------------------

void Water::updateWetPass() {
    const bool want = settings_.enabled && settings_.oceanWaves && !failed_ && !wetFailed_;
    if (want && wetPassId_ == 0) {
        wetPassId_ = dev_.addPass(d3d9::inject::Point::AfterOpaque, "water.wet_shore", [this](d3d9::PassContext& ctx) { recordWet(ctx); },
                                  this);
        wetFrames_ = 0;
    } else if (!want && wetPassId_ != 0) {
        dev_.removePass(wetPassId_);
        wetPassId_ = 0;
    }
}

void Water::destroyWetPipeline() {
    vk::Context* ctx = &dev_.ctx();
    const VkPipeline p = wetPipeline_;
    vk::PassLayout layout = wetLayout_;
    if (p != VK_NULL_HANDLE || layout.valid()) {
        dev_.frames().retire([ctx, p, layout]() mutable {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(ctx->device(), p, nullptr);
            layout.destroy(*ctx);
        });
    }
    wetPipeline_ = VK_NULL_HANDLE;
    wetLayout_ = vk::PassLayout{};
    wetFormat_ = VK_FORMAT_UNDEFINED;
}

bool Water::ensureWetPipeline(VkFormat target) {
    if (wetFailed_) return false;
    vk::Context& ctx = dev_.ctx();
    if (!wetLayout_.valid() && !wetLayout_.create(ctx, dev_.bindless(), {})) {
        wetFailed_ = true;
        return false;
    }
    if (wetPipeline_ != VK_NULL_HANDLE && wetFormat_ == target) return true;
    if (wetPipeline_ != VK_NULL_HANDLE) {
        const VkPipeline old = wetPipeline_;
        vk::Context* c = &ctx;
        dev_.frames().retire([c, old] { vkDestroyPipeline(c->device(), old, nullptr); });
        wetPipeline_ = VK_NULL_HANDLE;
    }
    vk::PassGraphicsDesc gd;
    gd.vs = shader::embedded::spirv("debug/fullscreen.vert");
    gd.fs = shader::embedded::spirv("water/wet_shore.frag");
    gd.color = target;
    // dst * src.a + src.rgb: darkens the scene (keeps the engine's exposure), adds the wet film's sheen; alpha kept
    gd.blend.enable = 1;
    gd.blend.srcColor = VK_BLEND_FACTOR_ONE;
    gd.blend.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
    gd.blend.srcAlpha = VK_BLEND_FACTOR_ZERO;
    gd.blend.dstAlpha = VK_BLEND_FACTOR_ONE;
    gd.name = "water.wet_shore";
    wetPipeline_ = vk::createPassGraphicsPipeline(ctx, wetLayout_.layout(), gd);
    wetFormat_ = target;
    if (wetPipeline_ == VK_NULL_HANDLE) {
        spdlog::error("enh: water: wet shore pipeline failed, the beach stays dry this session");
        wetFailed_ = true;
        return false;
    }
    return true;
}

void Water::recordWet(d3d9::PassContext& ctx) {
    d3d9::Texture* scene = ctx.sceneHdr;
    // the frame's first main view rendering into the scene target, the sea's grid drawn last frame, the shore field baked
    if (ctx.mainView != 1 || scene == nullptr || ctx.target != scene || ctx.depthMap == nullptr || !field_) return;
    const std::uint64_t frame = dev_.stats().frames;
    if (lastGridFrame_ == 0 || frame > lastGridFrame_ + 2) return;
    vk::Image& target = scene->image();
    if (!ensureWetPipeline(target.format())) return;
    VkCommandBuffer cmd = ctx.cmd;
    if (!dev_.readable(ctx.depthMap, cmd) || !dev_.readable(field_.get(), cmd)) return;
    const std::uint32_t w = scene->desc().width, h = scene->desc().height;
    WetShorePush push{};
    push.h.frame = ctx.frameBlock;
    push.h.passId = wetPassId_;
    push.depthTex = ctx.depthMap->bindlessSlot();
    push.fieldTex = field_->bindlessSlot();
    push.smpLinear = dev_.samplerFor(true, true);
    push.smpPoint = dev_.samplerFor(false, true);
    for (int i = 0; i < 4; ++i) push.shoreMap[i] = fieldMapping_[static_cast<std::size_t>(i)];
    push.ocean[0] = seaLevel_;
    push.ocean[1] = kLowering;
    push.ocean[2] = kSwash;
    push.ocean[3] = kCoastBand;
    push.params[0] = static_cast<float>(std::fmod(seaClock(), 16384.0));
    push.params[1] = kWetHeight;
    push.params[2] = kWetDarkening;
    // the FFT ocean's swash (in phase with its waves rolling in): the film the draining water leaves on the sand
    if (fftWanted() && ocean_ && ocean_->ready()) {
        push.params[3] = shoreWave()[0];
        d3d9::Texture* sky = dev_.imageWithRole(d3d9::scene::ImageRole::SkyEnvCube);
        if (sky != nullptr && !sky->isNull() && sky->kind() == vk::TextureKind::Cube && dev_.readable(sky, cmd)) {
            push.sky[0] = sky->bindlessSlot();
            push.sky[1] = dev_.samplerFor(true, true);
        }
    }

    target.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.attachmentView(dev_.ctx(), 0, 0);
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wetPipeline_);
    wetLayout_.bindSet0(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev_.bindless());
    wetLayout_.push(cmd, &push, sizeof(push));
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (wetFrames_++ == 0) spdlog::info("enh: water: wet shore pass at {}: {}x{}", d3d9::inject::name(ctx.point), w, h);
}

}  // namespace rvk::enh
