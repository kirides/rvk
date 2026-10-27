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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "native/frame_desc.h"
#include "native/static_compose.h"

namespace rvk::hal::vulkan {
class HalDevice;
}

namespace rvk::native {

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
        Count
    };
    static constexpr std::size_t kGroups = static_cast<std::size_t>(Group::Count);
    static const char* name(Group g);
    void setMode(Group g, bool takeOver, bool compare);
    // entity overlay passes composed (native.scene_ops_groups bit 2, while scene ops are on): else such ops stay the engine's
    void setOverlays(bool on) { overlays_ = on; }
    bool active() const;

    // driven by the frame tap (native/frame_tap): a main view's Execute begins / ends; the engine enters an op's slot
    void viewBegin(void* db, void* desc);
    void viewEnd();
    void opEnter(const Op& op);
    // eCDecalAdmin::RenderDecals of the main view begins (inside its draw tag, before the engine's code) / ends
    void decalsBegin(void* admin, void* db);
    void decalsEnd();
    // an engine draw of the main view (op = the executing op, null: none); true = drop it (we drew the op ourselves)
    bool engineDraw(const d3d9::DrawTapInfo& draw, const Op* op);
    // true while our own draws are being issued (the tap must not attribute them to the engine)
    bool issuing() const { return issuing_; }
    // one of our own draws reached prepareDraw: its device state is compared with the composed draw (the recorder gets what
    // was composed; with compare on)
    void ownDraw(const d3d9::DrawTapInfo& draw);
    void report();

    enum class Skip : std::uint8_t {
        None, Kind, Overlay, ShaderClass, Version, InvalidShader, NotCompiled, Switch, Primitive, ImageOverride, Unreadable, PassKind,
        Sort, Texture, OverlayShader, DecalProxy, NotCreated, VisualShader, Count
    };
    static const char* name(Skip s);

private:
    struct Live {
        std::bitset<sc::kVsRegs> f;  // float registers in the CTAB
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
            std::uint32_t vsFirst = 232, psFirst = 6, vsCount = 0, psCount = 0;
            float vs[32][4]{};
            float ps[16][4]{};
            std::array<const void*, 16> slotImage{};  // sampler pool slot images (switch samplers update them)
            std::uint32_t slots = 0;
        };
        std::unordered_map<const void*, ShaderPools> shaders;
        std::array<const void*, 2> lightmapSlot{};  // admin s4 / s5 slot images (BindLightmaps)
    };
    // per main view, natively composed (camera, fog, viewport, obstacle)
    struct View {
        bool valid = false;
        const std::uint8_t* admin = nullptr;
        const std::uint8_t* gfx = nullptr;
        float vp[4][4]{}, dbvp[4][4]{}, invProj[4][4]{}, invView[3][4]{};
        float camPos[4]{};
        float fogC3[4]{}, fogC4[4]{};
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
        sc::Visual visual;
        const void* subMesh = nullptr;
        const void* material = nullptr;
        const void* meshResource = nullptr;
    };
    bool composeView(const std::uint8_t* admin);
    bool shaderClassOk(const void* shader);
    const Live& liveOf(const d3d9::Shader* s);
    d3d9::Shader* shaderOfElement(const void* element);
    d3d9::Texture* textureOfImageData(const void* imageData, bool& ok);
    Mirror::ShaderPools* poolsOf(const void* shader);
    void initMirror();
    // compose the op into ops_' draws; Skip::None on success
    Skip composeOp(const Op& op);
    Skip composeZ(const Op& op);
    Skip composeSolid(const Op& op);
    Skip subMeshDraws(const std::uint8_t* res, const std::uint8_t* sub, std::uint32_t node, std::int32_t switchValue,
                      std::uint32_t combi, const std::uint8_t* illum, float fade, const float* world, bool instanced,
                      std::uint8_t flags, std::uint32_t batchCount);
    // eCShaderBase::OnGetPasses' version GetPasses for `owner` (the base shader, or an overlay shader) appended to `out`
    Skip shaderPasses(const std::uint8_t* owner, const sc::PassInputs& in, const std::uint8_t*& version, std::vector<sc::Pass>& out);
    // ExecutePass: `shader` = the base shader (SetSwitch, depth bias), `owner` = the pass's version owner (states, pools, samplers)
    Skip passDraw(const std::uint8_t* shader, const std::uint8_t* owner, const std::uint8_t* version, const sc::Pass& pass,
                  const Geom& geometry, bool overlay);
    Skip specialPassDraw(const std::uint8_t* shader, const Geom& geometry);
    bool geometryOf(const std::uint8_t* res, Geom& g, int stream3);  // stream3: 0 off, 1 on, -1 as the record has it
    sc::DrawDesc& drawFrom(const Geom& g);
    void constantsInto(sc::DrawDesc& d, const std::uint8_t* shader, const std::uint8_t* biasShader = nullptr);
    Skip materialSamplers(sc::DrawDesc& d, const std::uint8_t* shader, const void* psElement);
    void applySwitch(const std::uint8_t* shader, std::int32_t switchValue, Skip& skip);
    void writeWorld(std::uint32_t slot, const float* m);
    void writeAlpha(std::uint32_t slot, float a);
    void writeLighting(const sc::Pass& pass);
    std::uint32_t bindLightmaps(const std::uint8_t* res);
    void capture(const d3d9::DrawTapInfo& t, sc::DrawDesc& out);
    // the decals (static_ops_nonop.cpp)
    Skip composeDecals(const std::uint8_t* admin, const std::uint8_t* db);
    Skip nonOpPasses(const std::uint8_t* shader, const std::uint8_t* illum, std::uint32_t combi, const Geom& geometry);
    bool geometryOfVisual(const std::uint8_t* visual, Geom& g);
    bool verifyDecals();
    // a composed draw's vertex / index bytes against the engine buffer's (the vertex data the engine generated on the CPU)
    bool geometryBytesEqual(const sc::DrawDesc& c, const sc::DrawDesc& captured, std::string& why);
    void finishOp();
    void issue();

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
    std::unordered_map<const d3d9::Shader*, Live> live_;
    std::unordered_map<const void*, d3d9::Shader*> elementShader_;

    std::size_t issueIndex_ = 0;
    std::vector<std::uint8_t> ownKind_;
    // non-op paths (decals): the unit whose draws are composed in place of an op
    Op nonOp_;
    bool inNonOp_ = false;
    int decalsVerified_ = 0;
    using VecFn = void (*)(void* vector, const void* matrix);
    VecFn transform_ = nullptr, transformDirection_ = nullptr;
    int (*decalCmp_)(const void*, const void*) = nullptr;
    float decalAlphaScale_ = 255.0f, one_ = 1.0f;  // DAT_1809f17b4, DAT_1809f1698
    std::unique_ptr<d3d9::Buffer> ownVb_, ownIb_;    // take-over: our own copies of the decals' vertex / index data  // draws::Kind of our own draws (take-over + compare: checked against the engine's)
    std::vector<std::string> firstMismatches_;
    std::uint32_t mismatchLogged_ = 0;
    std::uint32_t fourLightsLogged_ = 0;
};

}  // namespace rvk::native
