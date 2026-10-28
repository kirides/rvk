#pragma once

// N5.1 "static ops" (docs/research/engine-frame/README.md 4 and "7. N5.1 result"; settings native.static_ops and
// native.static_ops_compare, Flag::Debug | NoAB, default off). The main view's z pre-pass and solid ops of static meshes
// (eCRenderContextMesh: single / whole-mesh z ops, sub-mesh and hierarchy-node solid ops, blended terrain layers, instanced
// ops) are composed from engine data at the op's slot entry: the engine's own translated shaders of the op's permutation,
// every constant register the shaders read, the render states, samplers, textures, streams and the draw ranges, by the
// engine's rules (native/static_compose.*).
//
// Compare (native.static_ops_compare): every draw the engine then makes for the op is diffed against the composed draw of
// the same position (rvk.log `native: static ops: compare ...` every 5 s, the first mismatches named; -LogFrame per op).
// Take-over (native.static_ops): the composed draws are issued by us at the op's slot entry (the device state is saved and
// restored around them, so the engine's own state caches stay valid) and the engine's draws of that op are dropped at
// prepareDraw; the engine's code still runs for every op (requests, LRU stamps, pools, lightmap slots), so its side effects
// are untouched. With both settings on, the dropped engine draws are still compared with what was drawn instead.
//
// N5.2 "scene ops" (README.md "8. N5.2 result"; settings native.scene_ops / native.scene_ops_compare, the groups in
// native.scene_ops_groups): the same machinery for the alpha and refracted lists' mesh ops (blend modes 2-8, the
// EnableAlphaZWrite z-fill + colour pair, fading statics; the refracted water through the enhanced water's DrawReplacer, which
// acts on the composed draw as on the engine's) and the entity overlay passes (burned / frozen / highlighted: the overlay
// materials' passes after the base passes, eCShaderBase::OnGetPasses 0x6d3170).
//
// Off (all settings false): nothing runs beyond the frame tap's pass-through (and the tap itself only when one of them or
// native.frame_tap is on). Render thread only.

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "native/frame_desc.h"
#include "native/static_compose.h"

namespace rvk::hal::vulkan {
class HalDevice;
}

namespace rvk::native {

// a pointer key's hash (multiplicative, high bits folded down: the map masks the low bits) instead of std::hash's FNV over its
// 8 bytes (O1: the per-draw shader / pool lookups)
struct PtrHash {
    std::size_t operator()(const void* p) const noexcept {
        const std::uint64_t x = (reinterpret_cast<std::uintptr_t>(p) >> 3) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(x ^ (x >> 29));
    }
};

class StaticOps {
public:
    explicit StaticOps(hal::vulkan::HalDevice& hal);
    ~StaticOps();
    StaticOps(const StaticOps&) = delete;
    StaticOps& operator=(const StaticOps&) = delete;

    // the op groups, each with its own take-over / compare switches and counts
    enum class Group : std::uint8_t {
        Static,  // N5.1: z pre-pass + solid mesh ops (native.static_ops)
        Mesh,    // N5.2: alpha + refracted mesh ops (native.scene_ops, group bit 1)
        Decals,  // N5.2: eCDecalAdmin::RenderDecals (group bit 4)
        Sky,     // N5.2c: the sky dome op (list +0x1530: stars, sun, moon, dome; group bit 8)
        Billboards,  // N5.2c: eCBillboardAdmin::RenderBillboards, the opaque billboards' queue (group bit 16)
        SkyCube,     // N5.2c: the sky environment cube's faces (eCSkydome_PS::Render with the cube camera; group bit 32)
        Actors,      // N5.3: the skinned actors' ops (eCRenderContextAnimation2: z / solid / alpha / refracted lists; group bit 64)
        Trees,       // N5.4: SpeedTree ops (branches, fronds, leaf cards / meshes) + the tree billboards' ops (z / solid; group bit 128)
        Vegetation,  // N5.4: the grass / vegetation ops (eCRenderContextVegetation: z / solid; group bit 256)
        ShadowViews, // N5.5: every op of the shadow maps' z lists (ExecuteShadowMap: sun cascades, terrain map, point-light atlas
                     // cells; meshes, skinned actors, SpeedTrees, ...: group bit 512)
        Particles,   // N5.6: the particle emitters' alpha ops (eCParticleRenderContext: CPU quads; group bit 4096)
        Strips,      // N5.6: the strips' (trails) and light streaks' alpha ops (eCStripRenderContext, eCLightStreaksRenderContext;
                     // group bit 8192)
        Precipitation,  // N5.6: rain / snow (eCPrecipitationRenderContext, the late list +0x1548; group bit 16384)
        Count
    };
    static constexpr std::size_t kGroups = static_cast<std::size_t>(Group::Count);
    static const char* name(Group g);
    void setMode(Group g, bool takeOver, bool compare);
    // entity overlay passes composed (native.scene_ops_groups bit 2, while scene ops are on): else such ops stay the engine's
    void setOverlays(bool on) { overlays_ = on; }
    bool active() const;

    // driven by the frame tap (native/frame_tap): a main view's Execute begins / ends; the engine enters an op's slot
    void viewBegin(void* db, void* desc, const FrameDesc& view);
    void viewEnd();
    // true = lean (O2a): the op was composed and issued and its side effects done, the engine's slot is not to be run
    bool opEnter(const Op& op);
    // eCDecalAdmin::RenderDecals / eCBillboardAdmin::RenderBillboards of the main view begins (inside its draw tag, before the
    // engine's code) / either ends; true = lean (the original not to be called)
    bool decalsBegin(void* admin, void* db);
    bool billboardsBegin(void* admin);
    // eCSkydome_PS::Render of a sky cube face (UpdateSkydomeEnvCubemap, outside Execute: the cube camera +0x3c0) begins; its
    // draws (view SkyCube) go to cubeDraw; nonOpEnd ends it; true = lean
    bool skyCubeBegin(void* sky);
    // O2a lean ops (native.lean, native.lean_check; docs/research/engine-frame/opt-plan.md "O2a result"): for an op of the
    // mesh-like groups (static, alpha / refracted mesh, overlays, decals, sky, billboards, sky cube) that was composed and
    // issued, the engine's slot / non-op code is not run; its non-drawing side effects are run here instead (resource touches,
    // lightmaps, stream 3, the instance sort, the billboard queue reset, the sky's cloud fields and first-use creation:
    // opt-side-effects-mesh.md 7). check = N: every N-th frame the engine's code runs as before (its draws dropped, the
    // compare active), 0 never.
    void setLean(bool on, int check);
    bool inSkyCube() const { return inSkyCube_; }
    void nonOpEnd();
    // an engine draw of the main view (op = the executing op, null: none); true = drop it (we drew the op ourselves)
    bool engineDraw(const d3d9::DrawTapInfo& draw, const Op* op);
    // true while our own draws are being issued (the tap must not attribute them to the engine)
    bool issuing() const { return issuing_; }
    // one of our own draws reached prepareDraw: its device state is compared with the composed draw (the recorder gets what
    // was composed; with compare on)
    void ownDraw(const d3d9::DrawTapInfo& draw);
    void report();
    // N5.8 coverage: why the engine's draws of `op` (null: of the non-op path running) stay the engine's (empty: no reason
    // recorded, i.e. the op was not met by the composition)
    std::string_view leftReason(const Op* op) const;

    enum class Skip : std::uint8_t {
        None, Kind, Overlay, ShaderClass, Version, InvalidShader, NotCompiled, Switch, Primitive, ImageOverride, Unreadable, PassKind,
        Sort, Texture, OverlayShader, DecalProxy, NotCreated, VisualShader, SkyCamera, SkyRebuilt, Bones, Shadow, Simulated, Instanced, Count
    };
    static const char* name(Skip s);

private:
    struct Live {
        std::bitset<sc::kVsRegs> f;  // float registers in the CTAB
        std::array<std::uint64_t, 4> words{};  // the same as word masks (bit r of word r / 64)
        std::uint32_t boolInt = 0;   // bool / int registers in the CTAB
    };
    struct GroupState {
        bool takeOver = false, compare = false;
        sc::CompareCounts window, total;
        std::array<std::uint64_t, static_cast<std::size_t>(Skip::Count)> skips{}, skipsTotal{};
        std::uint64_t issuedDraws = 0, issuedOps = 0, droppedDraws = 0, ownChecked = 0, ownMismatch = 0;
        // the lights of the composed sub-mesh draws (dynamic list +0x34 + static illum +0xd: 0..4; 4 = an additive light pass)
        std::array<std::uint64_t, 5> lights{};
        bool active() const { return takeOver || compare; }
    };
    // the engine's constant state at the op's entry, updated as the engine path would write it (C-materials.md 3)
    struct Mirror {
        float adminVs[31][4]{};   // admin VS pool c0..c30
        float adminPs[6][4]{};    // admin PS pool c0..c5
        float global[201][4]{};   // ms_pGlobalShaderConstants VS pool c31..c231
        float lighting[24][4]{};  // the lighting pool c14..c37
        struct ShaderPools {
            std::uint64_t epoch = 0;  // read in this mirror epoch (re-read when it differs)
            std::uint32_t vsFirst = 232, psFirst = 6, vsCount = 0, psCount = 0;
            float vs[32][4]{};
            float ps[16][4]{};
            std::array<const void*, 16> slotImage{};  // sampler pool slot images (switch samplers update them)
            std::uint32_t slots = 0;
        };
        std::unordered_map<const void*, ShaderPools, PtrHash> shaders;
        std::array<const void*, 2> lightmapSlot{};  // admin s4 / s5 slot images (BindLightmaps)
    };
    // O1: the mirror is our own register file, carried across ops, views and frames like the device's (the engine's pools are
    // read once; then only the writes of engine code we do not compose are merged in: native.mirror, docs/research/engine-frame/
    // opt-plan.md "O1 result"). Mode 0 persistent, 1 the engine's pools re-read per view, 2 per op (the N5.x behaviour).
public:
    void setMirrorMode(int mode) { mirrorMode_ = mode; }
    // native.coverage: the per-draw bookkeeping beyond the compare (see FrameTap)
private:
    int mirrorMode_ = 0;
    bool mirrorValid_ = false;      // global / lighting / lightmap slots read (else: read whole at the next use)
    std::uint64_t mirrorEpoch_ = 1;  // the shaders' pools and the admin pools are re-read once per epoch (a view; mode 2: an op)
    std::uint64_t adminEpoch_ = 0;
    static constexpr std::size_t mirrorShaderCap_ = 8192;
    const void* mirrorGlobalSrc_ = nullptr;
    const void* mirrorLightingSrc_ = nullptr;
    // the material admin the lightmap slots are read from (the last view's: a view's first ops run before composeView sets view_)
    const std::uint8_t* poolAdmin_ = nullptr;
    // engine code we do not compose ran (an op left to the engine, a non-op unit not composed): the engine's pools as they were
    // before it; the registers it changed are merged into the mirror at the next use
    struct EngineSnap {
        float global[201][4]{};
        float lighting[24][4]{};
        std::array<const void*, 2> lightmapSlot{};
    };
    EngineSnap leftSnap_, leftNow_;
    bool leftPending_ = false;
    std::uint64_t mergedRegs_ = 0, mergedLeft_ = 0;  // diagnostics: registers merged / left units seen
    void markLeft();
    bool readEnginePools(float (*global)[4], float (*lighting)[4], std::array<const void*, 2>& lightmaps) const;
    // per main view, natively composed (camera, fog, viewport, obstacle)
    struct View {
        bool valid = false;
        const std::uint8_t* admin = nullptr;
        const std::uint8_t* gfx = nullptr;
        float vp[4][4]{}, dbvp[4][4]{}, invProj[4][4]{}, invView[3][4]{};
        float camPos[4]{};
        float fogC3[4]{}, fogC4[4]{};
        bool fog = true;  // the view executes its fog (eCGfxFog::Execute); false: PS c3 / c4, VS c30 as the admin's pools hold them
        float obstacleX = 0, obstacleY = 0;
        bool zColorWrite = true;
        int dbOrder = -1;  // the depth-bias product's order matched against the admin's copy (0 P x V, 1 V x P, -1 neither)
    };

    // what a draw takes from the mesh resource's visual + the sub-mesh (eCGfxVisual, its eCAPIPrimitive record)
    struct Geom {
        const void* decl = nullptr;
        std::array<sc::DrawDesc::Stream, 4> streams{};
        const void* indices = nullptr;
        std::uint32_t minIndex = 0, numVertices = 0, startIndex = 0, primCount = 0;
        std::uint32_t primType = 4;   // the static path: an indexed triangle list, base vertex 0; skinned primitives: the record's
        std::int32_t baseVertex = 0;
        sc::Visual visual;
        const void* subMesh = nullptr;
        const void* material = nullptr;
        const void* meshResource = nullptr;
    };
    // camera null: the database's (db +0x14c0 -> +0x1738), with its fog; else that camera, no fog of its own (the sky cube)
    bool composeView(const std::uint8_t* admin, const std::uint8_t* camera = nullptr);
    bool shaderClassOk(const void* shader);
    const Live& liveOf(const d3d9::Shader* s);
    d3d9::Shader* shaderOfElement(const void* element);
    d3d9::Texture* textureOfImageData(const void* imageData, bool& ok);
    struct TexCacheEntry {
        const void* api = nullptr;
        d3d9::Texture* texture = nullptr;
    };
    static constexpr std::uint32_t kTexCacheBits = 12;
    std::array<TexCacheEntry, 1u << kTexCacheBits> texCache_{};
    std::uint64_t texCacheGen_ = ~0ull;
    Mirror::ShaderPools* poolsOf(const void* shader);
    void initMirror();
    // compose the op into ops_' draws; Skip::None on success
    Skip composeOp(const Op& op);
    Skip composeZ(const Op& op);
    Skip composeSolid(const Op& op);
    Skip subMeshDraws(const std::uint8_t* res, const std::uint8_t* sub, std::uint32_t node, std::int32_t switchValue,
                      std::uint32_t combi, const std::uint8_t* illum, float fade, const float* world, bool instanced,
                      std::uint8_t flags, std::uint32_t batchCount);
    // eCShaderBase::GetPasses 0x6d1ee0 (-> OnGetPasses: the base shader's passes, then the overlay materials' for flags 1 / 2 / 4)
    // + ExecutePass per pass over `geometry` (the sub-mesh draws, the skinned primitives); world: for the 4-light log only
    Skip materialDraws(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, bool instanced, std::int32_t switchValue,
                       std::uint8_t flags, const Geom& geometry, const float* world);
    // eCShaderBase::OnGetPasses' version GetPasses for `owner` (the base shader, or an overlay shader) appended to `out`
    Skip shaderPasses(const std::uint8_t* owner, const sc::PassInputs& in, const std::uint8_t*& version, std::vector<sc::Pass>& out);
    // ExecutePass: `shader` = the base shader (SetSwitch, depth bias), `owner` = the pass's version owner (states, pools, samplers)
    Skip passDraw(const std::uint8_t* shader, const std::uint8_t* owner, const std::uint8_t* version, const sc::Pass& pass,
                  const Geom& geometry, bool overlay);
    // ExecuteSpecialPass(option) 0x6d1cd0: the special pass type * 2 + option (SpeedTree z passes: option 1)
    Skip specialPassDraw(const std::uint8_t* shader, const Geom& geometry, bool option = false);
    // eCMeshResource2::RenderZPass 0x7603b0 (world null: no SetWorldMatrix / SetGlobalAlpha; sub 0xff: the whole mesh)
    Skip meshZDraws(const std::uint8_t* res, std::uint32_t sub, std::uint32_t node, const float* world, bool option);
    Skip adminSamplers(sc::DrawDesc& d, const std::uint8_t* owner, const d3d9::Shader* ps);
    // O1: the engine state the samplers of every draw of an op read, read once per op (the composition runs at the op's entry:
    // nothing of the engine runs between its draws): the admin's sampler slots / images +0x248..+0x350, the per-stage override
    // images, the gfx admin's default filter
    struct SamplerSnap {
        std::uint64_t serial = ~0ull;
        const std::uint8_t* admin = nullptr;
        std::uint8_t block[0x108]{};  // admin +0x248 ..
        std::array<const void*, 16> overrides{};
        std::uint32_t defFilter = 0;
        template <class T>
        T at(std::uint32_t off) const {
            T v;
            std::memcpy(&v, block + (off - 0x248), sizeof(T));
            return v;
        }
    } snap_;
    std::uint64_t opSerial_ = 0;  // bumped per op / unit (finishOp)
    const SamplerSnap& samplerSnap();
    bool geometryOf(const std::uint8_t* res, Geom& g, int stream3);  // stream3: 0 off, 1 on, -1 as the record has it
    sc::DrawDesc& drawFrom(const Geom& g);
    // defaultClass: eCShaderDefault's register map (admin c0..c30, global c31..c231, the shader's pools from c232 / PS c6..c13,
    // the lighting pool PS c14..c37); else eCShaderBase's (no global constants, no lighting: the shader's pools where they are)
    void constantsInto(sc::DrawDesc& d, const std::uint8_t* shader, const std::uint8_t* biasShader = nullptr, bool defaultClass = true);
    Skip materialSamplers(sc::DrawDesc& d, const std::uint8_t* shader, const void* psElement);
    void applySwitch(const std::uint8_t* shader, std::int32_t switchValue, Skip& skip);
    void writeWorld(std::uint32_t slot, const float* m);
    void writeAlpha(std::uint32_t slot, float a);
    void writeTreeInstance(std::uint32_t slot, const float v[2]);  // SetSpeedTreeInstanceData 0x6c9eb0: c32 + 4 slot .xy
    void writeLighting(const sc::Pass& pass);
    std::uint32_t bindLightmaps(const std::uint8_t* res);
    void capture(const d3d9::DrawTapInfo& t, sc::DrawDesc& out);
    // the decals (static_ops_nonop.cpp)
    Skip composeDecals(const std::uint8_t* admin, const std::uint8_t* db);
    Skip nonOpPasses(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, const Geom& geometry,
                     std::int32_t switchValue = 0);
    // a non-op path's unit begins (false: the group is off / the checks failed) / its composition's result (issued on take-over)
    bool nonOpGroupBegin(Group group, void* admin);
    void nonOpGroupComposed(Skip s);
    bool geometryOfVisual(const std::uint8_t* visual, Geom& g);
    bool verifyDecals();
    // a composed draw's vertex / index bytes against the engine buffer's (the vertex data the engine generated on the CPU)
    bool geometryBytesEqual(const sc::DrawDesc& c, const sc::DrawDesc& captured, std::string& why);
    // the opaque billboards' queue (static_ops_nonop.cpp)
    Skip composeBillboards(const std::uint8_t* admin);
    // N5.8: a billboard set's alpha op (RenderAlpha 0x4f4c70 -> eCBillboardAdmin::RenderBillboard(..., alpha = true, fade) 0x4f0c00)
    Skip composeBillboardAlpha(const Op& op);
    const std::uint8_t* billboardAdmin_ = nullptr;  // the eCBillboardAdmin of the last RenderBillboards (RenderAlpha's admin)
    std::uint64_t billboardAlphaOps_ = 0, billboardAlphaDraws_ = 0;
    std::uint32_t streakLogged_ = 0;
    bool verifyBillboards();
    // the skinned actors' ops (static_ops_actors.cpp)
    bool verifyActors();
    Skip composeActor(const Op& op);
    // the bone b's matrix as the shader gets it: the palette entry the engine reads (stored this frame) or composed as
    // UpdateRenderMatrices will (X[i] *= W[i] with the engine's own affine product)
    bool paletteMatrix(const std::uint8_t* wrapper, std::uint32_t bone, float out[16]);
    void writeBone(std::uint32_t slot, const float* m);
    // the SpeedTree / tree billboard / vegetation ops (static_ops_trees.cpp)
    bool verifyTrees();
    Skip composeTree(const Op& op);
    Skip composeTreeBillboard(const Op& op);
    Skip composeVegetation(const Op& op);
    // the view from a material shader's admin (+0x220) + the mirror (false: another admin / not composable)
    bool viewAndMirror(const std::uint8_t* shader);
    // the wind registers of the admin's VS pool as their writers set them: c14..c29 (eCShaderMaterialAdmin::SetSpeedTreeData
    // 0x6b7df0 from eCSpeedTreeAdmin +0xac / +0x6c and the application camera), c12 (eCVegetationAdmin::Process 0x7016b0)
    void treeWind();
    void grassWind();
    // the sky dome op (static_ops_sky.cpp)
    Skip composeSky(const Op& op);
    Skip composeSkyDraws(const std::uint8_t* sky, const std::uint8_t* camera);
    bool verifySky();
    // the camera matrices eCGfxCamera::Execute -> SetCamera uploads for view / projection / depth-bias projection, computed with
    // the engine's own bCMatrix functions (the sky's camera hack and its restore): into view_ and the mirror's admin VS c4..c10
    void setCamera(const float view[16], const float proj[16], const float dbProj[16]);
    // eCSkyObject::Render 0x6e25e0 of a sky object (stars / sun / moon); bb: the billboards FillVertexBuffer writes (null: the
    // object's array), renderCount: the billboards drawn (0: the record's primitive count / 2)
    Skip skyObjectDraws(const std::uint8_t* object, const float* world, const std::uint8_t* bb, std::uint32_t renderCount);
    Skip skyDomeDraw(const std::uint8_t* shader, const std::uint8_t* visual);
    int blendModeOf(const std::uint8_t* owner) const;
    // N5.6 (static_ops_fx.cpp): particles, strips, light streaks, precipitation
    bool verifyFx();
    Skip composeFx(const Op& op);
    Skip composeStrip(const Op& op);
    Skip composeStreaks(const Op& op);
    Skip composeParticle(const Op& op);
    Skip composePrecipitation(const Op& op);
    int fxVerified_ = 0;
    // a shader class's own global pool (eCShaderParticle::ms_pGlobalShaderConstants +0x20): its registers as the op's path writes
    // them; count 0 = none (constantsInto, non-default classes)
    float fxPool_[32][4]{};
    std::uint32_t fxPoolFirst_ = 31, fxPoolCount_ = 0, fxPoolNativeLast_ = 0;
    const std::uint8_t* particleGlobalVar_ = nullptr;  // &eCShaderParticle::ms_pGlobalShaderConstants (decoded from 0x6d5fc2)
    bool fogOverride_ = false;  // EnableFog (shader +0x2b0) as ApplyRenderStates writes it
    Skip particlePassDraw(const std::uint8_t* shader, const std::uint8_t* version, std::uint32_t key, const Geom& geometry, float alpha);
    bool precipitationRecords(const std::uint8_t* ps, std::uint32_t records, std::vector<std::uint8_t>& out);
    std::uint32_t precipLogged_ = 0;
    // the billboard records skyObjectDraws expands instead of the object's array (precipitation: re-done from the drops)
    const std::vector<std::uint8_t>* billboardsOverride_ = nullptr;
    bool particleVertices(const std::uint8_t* ps, const std::uint8_t* entity, std::vector<std::uint8_t>& out, std::uint32_t& quads);
    struct FxStats {
        std::uint64_t particleOps = 0, particleQuads = 0, particleSkipped = 0, particleEngineBytes = 0, particleZeroQuads = 0, particleSorted = 0;
        std::array<std::uint64_t, 6> orient{};    // PS +0x170: 0..4, other
        std::array<std::uint64_t, 2> coords{};    // PS +0xb0: world / entity-relative
        std::uint64_t rotated = 0, frameBlend = 0, lit = 0, instancedEmitters = 0;
        std::uint64_t stripOps = 0, stripSegments = 0, streakOps = 0, precipOps = 0, precipDrops = 0, precipOff = 0;
        // particle ops left to the engine because no particle was alive at the entry, and those of them the engine then drew
        std::uint64_t simulatedOps = 0, simulatedDrew = 0;
        // the precipitation's billboard records re-done from the drops vs the engine's (eCPrecipitation_PS::Update's fill)
        std::uint64_t precipRecordsEqual = 0, precipRecordsDiffer = 0;
    } fxWindow_, fxTotal_;
    // a draw tag for the issued draws other than the slot's (particles: eCParticle_PS::Render's TagScope inside the slot)
    bool tagOverride_ = false;
    const Op* simulatedOp_ = nullptr;  // the last particle op skipped as Simulated (its engine draws are counted)
    bool simulatedDrew_ = false;
    d3d9::draws::Tag tagOverrideValue_{};
    void finishOp();
    void issue();
    // ---- O2a lean ops (static_ops_lean.cpp) ----
    bool lean_ = false;
    int leanCheck_ = 0;
    std::uint64_t leanFrameNo_ = ~0ull;
    sc::LeanFrame leanFrameKind_ = sc::LeanFrame::Lean;
    bool leanFrameOn_ = false;
    void leanUpdateFrame();
    // the compare runs (with lean on: only on check frames; on the other frames the engine's draws of the ops it still runs see
    // the device the lean ops left, not the engine's own: opt-plan.md "O2a result")
    bool comparing() {
        if (g_ == nullptr || !g_->compare) return false;
        if (!lean_) return true;
        leanUpdateFrame();
        return leanFrameKind_ == sc::LeanFrame::Check;
    }
    int leanVerified_ = 0;  // the engine functions the lean path calls: 1 resolved, -1 not (never lean)
    int leanAnimVerified_ = 0;  // O2b: the animated / simulated kinds' engine functions and sites (1 ok, -1: those groups not lean)
    bool leanFrame();       // lean on and this frame is not a check frame
    bool leanGroup(Group g) const;
    bool verifyLeanAnim();
    // the op being composed is a lean candidate: the side effects its engine code would have are collected while composing
    // (leanBegin) and run when its draws were issued by us (leanCommit: true = the engine's code is skipped)
    bool leanCollect_ = false;
    // kBlockEdge: an engine path whose side effects the lean path does not reproduce (rare first-use / failure cases): the op keeps
    // the engine's code
    enum LeanBlock : std::uint32_t { kBlockVideo = 1, kBlockEdge = 2 };
    std::uint32_t leanBlock_ = 0;
    std::vector<const void*> leanTouches_, leanLightmaps_;
    struct LeanStream3 {
        void* primitive;
        bool on;
    };
    std::vector<LeanStream3> leanStream3_;
    bool slotSkipped_ = false;  // the current op's / unit's engine code was skipped
    void leanBegin(Group g);
    bool leanCommit();
    void leanAbort() { leanCollect_ = false; }
    void leanTouch(const void* resource) {
        if (leanCollect_ && resource != nullptr) leanTouches_.push_back(resource);
    }
    void leanImage(const void* resource, const void* imageData);  // a bound image: touch + video check + placeholder count
    void touchNow(const void* resource);  // eCResource2::RequestData(resource, 1) once per frame
    sc::TouchSet<8192> touched_;
    // first use / the S writes the engine's code makes before its draws (run before composing)
    void leanDecalsPre(const std::uint8_t* admin);
    void leanBillboardsPre(const std::uint8_t* admin);
    void leanSkyPre(const std::uint8_t* sky);
    void leanBillboardsPost(const std::uint8_t* admin);
    void leanSortInstances(const std::uint8_t* res);
    void leanStream3(const std::uint8_t* res, bool on);
    // ---- O2b: the animated / simulated kinds (opt-side-effects-anim.md; run where the engine's code runs them, before / while
    // composing; the ones that would change what a fallback slot sees are deferred to the commit) ----
    // eCAnimation_PS::Render: UpdateRenderMatrices(wrapper, frame id) (the palette + its stamp), then HardRenderActor2::Render's
    // touches of the HRA resource and (cached) the LOD primitive's material
    void leanActorPre(const std::uint8_t* wrapper, std::uint16_t primIdx);
    // eCRenderContextSpeedTree::Render / RenderZPass: the queue sorted in place at the render buffer's first op + its stamp = 0
    void leanTreeSort(const std::uint8_t* rb, bool zpass);
    // RenderBranches / Fronds / Leafs (+ z passes): MaskReference = 80 with the property notifies (when it is not 80 yet)
    void leanMaskRef(const std::uint8_t* shader);
    // eCParticle_PS::Render's first steps (GetMasterInstance, the in-slot OnProcess, the inactivity timer): false = Render returns
    bool leanParticlePre(const std::uint8_t* ps);
    // ApplyRenderStates' creation / touches (CreateMaterial, RequestData, CreateImage + the sampler material's SetImage)
    void leanParticleStates(const std::uint8_t* ps);
    // committed with the op: the vegetation cursor, a particle target's in-place depth sort + our vertex cache, a class pool's
    // registers (eCShaderParticle / eCShaderLightStreaks global pools: the engine's composition reads them per op)
    struct LeanVeg {
        const std::uint8_t* admin = nullptr;
        std::uint32_t cursor = 0;
    } leanVeg_;
    const std::uint8_t* leanParticleTarget_ = nullptr;
    struct LeanPool {
        const std::uint8_t* var = nullptr;  // the class's ms_pGlobalShaderConstants (its pool +0x38 = the registers from +0x24)
        std::uint32_t count = 0;
        float regs[32][4]{};
    } leanPool_;
    void leanPoolFrom(const std::uint8_t* var, std::uint32_t count);  // fxPool_'s first `count` registers, written at the commit
    // a particle target's vertices of this frame, made by us (the engine's UpdateVertices did not run: its +0x3b0 stays)
    struct ParticleCache {
        std::uint64_t frame = ~0ull;
        std::uint32_t quads = 0;
        std::vector<std::uint8_t> vb;
    };
    std::unordered_map<const void*, ParticleCache, PtrHash> particleCache_;
    std::vector<std::uint8_t> leanParticleVb_;
    std::uint32_t leanParticleQuads_ = 0;
    // engine functions of the O2b path (resolved by verifyLeanAnim)
    bool (*updateRenderMatrices_)(const void* wrapper, std::uint32_t frame) = nullptr;
    void* (*particleModule_)() = nullptr;
    void* (*getMasterInstance_)(void* module, const void* ps) = nullptr;
    void (*depthSortParticles_)(const void* ps) = nullptr;
    bool (*particleCreateVertices_)(const void* ps) = nullptr;
    bool (*particleCreateMaterial_)(const void* ps) = nullptr;
    bool (*particleCreateImage_)(const void* ps) = nullptr;
    bool (*stripCreateVertices_)(const void* ps) = nullptr;
    bool (*streaksCreateBuffers_)(const void* ps) = nullptr;
    const void* maskRefProperty_ = nullptr;  // the MaskReference property the tree paths' notifies name (0x97bb98)
    // O2b: on a lean frame the engine's global / lighting pools lag behind (the lean ops do not write them): before engine code
    // runs, ours are copied into them (so it starts from what lean off would have, and its changes are found by the diff)
    bool syncEnginePools();
    struct LeanStats {
        std::uint64_t frames = 0, checkFrames = 0;
        std::array<std::uint64_t, kGroups> skipped{}, kept{};
        std::uint64_t blockedVideo = 0, blockedEdge = 0, touches = 0, touchCalls = 0, lightmaps = 0, stream3 = 0, instanceSorts = 0, queueResets = 0;
        std::uint64_t placeholderBinds = 0, skyWrites = 0, creates = 0;
        std::int32_t queueMax = 0;
        // O2b
        std::uint64_t palettes = 0, treeSorts = 0, maskRefs = 0, vegCursors = 0, poolWrites = 0, poolSyncs = 0;
        std::uint64_t masters = 0, noMaster = 0, kicks = 0, kicksSpawned = 0, inactivity = 0, particleSorts = 0, particleCached = 0;
        std::uint64_t particleCreates = 0, stripCreates = 0, streakCreates = 0;
    } leanWindow_, leanTotal_;
    std::vector<const void*> placeholderImages_;  // distinct resources bound as the placeholder in the window (bounded)
    std::vector<std::string> placeholderNames_;   // the first ones' names, read while bound (a load may free them before the report)
    std::uint32_t videoLogged_ = 0, placeholderLogged_ = 0;
    void leanReport();
    // the cloud fields as the lean path last wrote them (reported; the sky object itself may be gone by then: a load)
    struct SkySnap {
        bool valid = false;
        float size[2]{}, scale[2][2]{}, dir[2]{}, scroller[2]{}, dome[4]{}, src[4]{};
    } leanSky_;
    // per-op working storage of the compositions, kept across ops (O1: no heap traffic per op once warm)
    struct Scratch {
        std::vector<sc::Pass> passes;  // materialDraws
        std::vector<sc::ZSub> zs;      // meshZDraws
        std::vector<sc::ZDraw> zplan;
        std::vector<std::uint8_t> inst;  // composeSolid's instances
        std::vector<sc::Instance> list;
        std::vector<sc::Batch> batches;
        std::vector<std::uint8_t> treeQueue;  // composeTree
        std::vector<sc::TreeInstance> treeInst;
        std::vector<sc::TreeBatch> treeBatches;
        std::vector<float> treeAlpha;
        std::vector<sc::GrassNode> grassPlan;  // composeVegetation
        std::vector<const std::uint8_t*> grassSources;
        std::vector<sc::GrassDraw> grassDraws;
        std::vector<std::uint8_t> grassBytes, grassSrc;
    } scr_;
    // issue's saved device registers (only the ranges the op's draws write are saved and restored)
    float savedVs_[sc::kVsRegs][4]{}, savedPs_[sc::kPsRegs][4]{};
    // N5.8 coverage: the last op met (or the non-op unit) and why it was left to the engine
    void opEnterImpl(const Op& op);
    enum : int { kLeftCompareOnly = 100, kLeftGroupOff, kLeftNotCovered, kLeftChecks };
    std::string_view leftName(Group g, int code);
    const Op* leftOp_ = nullptr;
    Group leftGroup_ = Group::Count;
    int leftCode_ = -1;  // a Skip, or kLeft*
    std::string_view leftWhy_;
    bool leftNonOp_ = false;
    std::string_view leftNonOpWhy_;
    std::map<int, std::string> leftNames_;

    hal::vulkan::HalDevice& hal_;
    d3d9::Device& dev_;
    std::array<GroupState, kGroups> groups_;
    GroupState* g_ = nullptr;  // the composed op's group (null: none)
    bool overlays_ = false;
    // the device's bool / int registers at the op's entry (the composition of those registers)
    std::array<std::uint32_t, 16> entryVsB_{}, entryPsB_{};
    std::int32_t entryVsI_[16][4]{};
    std::int32_t entryPsI_[16][4]{};
    int verified_ = 0;  // engine checks at first use: 1 ok, -1 failed (nothing composed)
    bool verify();
    const std::uint8_t* engine_ = nullptr;  // Engine.dll base
    const std::uint8_t* lightingPoolVar_ = nullptr;  // &DAT_180c62968 (decoded from FUN_1806cd1d0)
    const std::uint8_t* placeholderTable_ = nullptr;  // eCImageResource2's per-stage placeholder images (from 0x68c650)
    const std::uint8_t* imageOverride_ = nullptr;     // eCGfxImageData::Execute's per-stage override table (from 0x7551c0)
    using QsortFn = void (*)(void*, std::size_t, std::size_t, int (*)(const void*, const void*));
    QsortFn qsort_ = nullptr;
    int (*instanceCmp_)(const void*, const void*) = nullptr;

    void* db_ = nullptr;
    void* desc_ = nullptr;
    bool inView_ = false;
    // the view being executed (N5.5): the main view or a shadow map's ExecuteShadowMap (its eCShadowMapBase, eCDepthMap format +8)
    ViewKind viewKind_ = ViewKind::Main;
    const void* shadowMap_ = nullptr;
    std::int32_t shadowFormat_ = -1, cascade_ = -1;
    // the shadow views' compared / equal draws and ops per view kind x op kind (window, total)
    struct ShadowStats {
        std::array<std::array<std::uint64_t, kOpKindCount>, kViewKindCount> ops{}, draws{}, clean{}, composed{};
        std::array<std::uint64_t, kViewKindCount> views{};
        void add(const ShadowStats& o);
    } shadowWindow_, shadowTotal_;
    View view_;
    Mirror mirror_;
    std::unique_ptr<sc::DrawDesc> scratch_;    // a capture
    std::vector<std::unique_ptr<sc::DrawDesc>> draws_;  // the op's composed draws (reused storage)
    std::size_t drawCount_ = 0;
    sc::DrawDesc& newDraw();
    const Op* op_ = nullptr;       // the op whose draws were composed (null: none / not composed)
    bool taken_ = false;           // its draws were issued by us
    std::uint32_t engineDraws_ = 0;
    bool issuing_ = false;
    bool opDiffLogged_ = false;
    std::uint32_t frameLogged_ = 0;  // -LogFrame: composed draws described
    std::unordered_map<const d3d9::Shader*, Live, PtrHash> live_;
    std::unordered_map<const void*, d3d9::Shader*, PtrHash> elementShader_;

    std::size_t issueIndex_ = 0;
    std::vector<std::uint8_t> ownKind_;
    // non-op paths (decals): the unit whose draws are composed in place of an op
    Op nonOp_;
    bool inNonOp_ = false;
    bool inSkyCube_ = false;
    // the sky cube's faces: UpdateSkydomeEnvCubemap's BeginMaterial binds the admin stages, then SetRenderTarget(face) unbinds all
    // 16 stages (eCDX9Device / HalDevice::SetRenderTarget): s0 / s1 / s2 / s4 / s5 are empty at the faces' draws
    bool adminStagesUnbound_ = false;
    int decalsVerified_ = 0, billboardsVerified_ = 0, skyVerified_ = 0, actorsVerified_ = 0, treesVerified_ = 0;
    // N5.4 (static_ops_trees.cpp): the admins' getters (FUN_18025a6d0 eCSpeedTreeAdmin, FUN_180265df0 eCVegetationAdmin), the queue
    // comparators (SortInstances 0x6f2180, SortInstancesZPass 0x6f2120), the billboards' fixed tex-coord table (0xbfcbd0)
    void* (*treeAdmin_)() = nullptr;
    void* (*vegAdmin_)() = nullptr;
    int (*treeCmp_)(const void*, const void*) = nullptr;
    int (*treeCmpZ_)(const void*, const void*) = nullptr;
    const std::uint8_t* bbTexCoords_ = nullptr;
    // the MaskReference a path writes into a shader before its draws (SpeedTree: 80): null none
    const std::uint8_t* maskRefShader_ = nullptr;
    std::uint8_t maskRefOverride_ = 0;
    std::uint8_t maskRefOf(const std::uint8_t* owner) const;
    const void* leafsVtable_ = nullptr;  // ??_7eCShaderLeafs (0x9d7be8), accepted by shaderClassOk once verifyTrees checked its slots
    std::string skipWhy_;                // the reason of the last skip (diagnostics)
    const void* skipShader_ = nullptr;   // the vtable of the last ShaderClass skip's shader
    std::uint32_t treeSkipsLogged_ = 0;
    const std::uint8_t* pendingVersion_ = nullptr;  // diagnostics of uncompiled special passes
    std::size_t pendingIdx_ = 0;
    std::uint32_t pendingLogged_ = 0;
    // admin VS registers c0..c30 composed natively in this op (bit r; c0..c11 and c30 always)
    std::uint32_t nativeVs_ = 0;
    struct TreeStats {
        std::uint64_t ops = 0, zeroOps = 0, hardware = 0, software = 0, instances = 0, batches = 0, sortedCopies = 0, undrawnBatches = 0;
        std::array<std::uint64_t, 4> types{};  // branches, fronds, leaf cards, leaf meshes
        std::uint64_t leafLods = 0, wrongTransformation = 0, billboardOps = 0, billboardTexCoords = 0, shadowFrondsSkipped = 0;
        std::uint64_t grassOps = 0, grassZeroOps = 0, grassNodes = 0, grassInstances = 0, grassDraws = 0, grassWraps = 0, grassNotCached = 0, grassClockMoved = 0;
    } treeWindow_, treeTotal_;
    // the skinned actors (static_ops_actors.cpp): the frame admin getter eCAnimation_PS::Render calls first (frame id at +0x4c), the
    // affine product of UpdateRenderMatrices (FUN_1800a2710: a *= b)
    void* (*frameAdmin_)() = nullptr;
    void (*affineMul_)(float* a, const float* b) = nullptr;
    struct ActorStats {
        std::uint64_t ops = 0, palettesStored = 0, palettesComposed = 0, morphActive = 0, morphOps = 0, bodyParts = 0, bones = 0;
        std::array<std::uint64_t, 5> lod{};
        std::uint64_t emptySlot = 0;  // refracted-list ops: eCRenderContextAnimation2 has no RenderRefracted (0 draws)
        std::uint64_t overlayOps = 0;
        std::array<std::uint64_t, 3> overlayKinds{};  // burned, frozen, highlighted
    } actorWindow_, actorTotal_;
    std::uint32_t frameId_ = 0;
    int (*billboardCmp_)(const void*, const void*) = nullptr;
    // the BlendMode a non-op path writes into a shader before its passes (SetCurrentRenderStates reads +0x50): -1 none
    const std::uint8_t* blendOverrideShader_ = nullptr;
    std::int32_t blendOverride_ = -1;
    using MatInvertFn = void (*)(void* m);
    using MatMulFn = void* (*)(const void* a, void* ret, const void* b);
    using MatMulAssignFn = void* (*)(void* a, const void* b);
    using MatInvertedFn = void* (*)(const void* m, void* ret);
    MatInvertFn matInvert_ = nullptr;
    MatMulFn matMul_ = nullptr;
    MatMulAssignFn matMulAssign_ = nullptr;
    MatInvertedFn matInverted_ = nullptr;
    std::uint32_t skyLogged_ = 0;
    using VecFn = void (*)(void* vector, const void* matrix);
    VecFn transform_ = nullptr, transformDirection_ = nullptr;
    int (*decalCmp_)(const void*, const void*) = nullptr;
    float decalAlphaScale_ = 255.0f, one_ = 1.0f;  // DAT_1809f17b4, DAT_1809f1698
    std::unique_ptr<d3d9::Buffer> ownVb_, ownIb_;  // take-over: our own copies of the CPU-generated vertex / index data
    // take-over: the vegetation's instance data, appended within a frame (one ring copy per frame instead of one per DISCARD)
    static constexpr std::uint32_t kOwnInstBytes = 2u << 20;
    // take-over: our own dynamic VB for CPU-generated geometry on stream 0 (decals, billboards, sky objects, particles, strips, rain)
    static constexpr std::uint32_t kOwnVbBytes = 4u << 20;
    std::unique_ptr<d3d9::Buffer> ownInstVb_;
    std::uint32_t ownInstCursor_ = 0;
    std::uint64_t ownInstFrame_ = ~0ull;
    std::vector<std::uint32_t> ownOffset_;  // per issued draw: its instance data's offset in ownInstVb_
    std::vector<std::string> firstMismatches_;
    std::uint32_t mismatchLogged_ = 0;
    std::uint32_t fourLightsLogged_ = 0;
};

}  // namespace rvk::native
