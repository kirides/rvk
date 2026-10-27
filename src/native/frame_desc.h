#pragma once

// The native frame, stage N5.0 "op tap" (docs/research/engine-frame/README.md 4 and "N5.0 result"): the main view's render
// ops as eCFrustumDatabase::Execute (0x66e990) will run them, and the attribution of every HAL draw of that view to the op
// that issued it.
//
// A FrameDesc holds the seven op lists of one main-view Execute in Execute's order (z pre-pass +0x1518, solid +0x1500, debug
// +0x1560, the list +0x1530 (sky dome), refracted +0x1578, alpha +0x14e8, late alpha +0x1548; B-frustum-db.md 2.6), each in
// the order the engine executes it: the z, solid and alpha lists are qsorted inside Execute, the others run in request order.
// The engine side (native/frame_tap.cpp) copies the lists at Execute's entry and sorts the copies with the engine's own qsort
// and comparators; Attribution then checks, op by op, that the engine executes exactly that sequence and maps each draw to
// its op. Pure logic: no engine access, no Vulkan (unit tests: tests/native_tests.cpp). Render thread only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "d3d9/draws.h"
#include "d3d9/scene.h"

namespace rvk::native {

// the op lists of eCFrustumDatabase, in the order Execute runs them
enum class List : std::uint8_t { ZPass, Solid, Debug, Sky, Refracted, Alpha, Late, Count };
inline constexpr std::size_t kListCount = static_cast<std::size_t>(List::Count);
// their offsets in eCFrustumDatabase (bTValArray<eCRenderOp2>: vtable +0, data +8, count +0x10)
inline constexpr std::array<std::uint32_t, kListCount> kListOffset{0x1518, 0x1500, 0x1560, 0x1530, 0x1578, 0x14e8, 0x1548};
// the render-context vtable slot Execute calls for an op of the list (0 Render, 1 RenderAlpha, 2 RenderRefracted, 3 RenderZPass)
inline constexpr std::array<std::uint8_t, kListCount> kListSlot{3, 0, 0, 0, 2, 1, 1};
// the lists Execute qsorts (z: comparator 0x66e670, solid: 0x66e6a0, alpha: 0x66e600); the others run in request order
inline constexpr std::array<bool, kListCount> kListSorted{true, true, false, false, false, true, false};

const char* name(List l);
// the phase pass the list's draws happen in (frame-phases.md: the refracted ops mark their own pass)
d3d9::scene::Pass listPass(List l);

// the render context class of an op (from the context's vtable; D-contexts.md 0.1)
enum class OpKind : std::uint8_t {
    Unknown,
    Mesh,           // eCRenderContextMesh
    Animation,      // eCRenderContextAnimation2 (skinned)
    SpeedTree,      // eCRenderContextSpeedTree
    SpeedTreeBB,    // eCRenderContextSpeedTreeBB
    Vegetation,     // eCRenderContextVegetation
    Particle,       // eCParticleRenderContext
    Strip,          // eCStripRenderContext
    LightStreaks,   // eCLightStreaksRenderContext
    Billboard,      // eCBillboardRenderContext
    Precipitation,  // eCPrecipitationRenderContext
    Skydome,        // the sky dome's context
    Debug,          // eCRenderContextDebug
    Count
};
inline constexpr std::size_t kOpKindCount = static_cast<std::size_t>(OpKind::Count);
const char* name(OpKind k);

// eCRenderOp2 (0x18 bytes, B-frustum-db.md 3; Create 0x67f680)
struct RawOp {
    const void* context = nullptr;  // eCRenderContext*
    std::uint32_t desc = 0;         // per context: sub-mesh | node << 8 (0xff whole mesh, 0xffffff00 | i instanced), ...
    float fade = 1.0f;              // global alpha (alpha / refracted / z ops carry the request's fade)
    std::uint32_t key = 0;          // 24-bit view depth key
    std::uint32_t sortId = 0;       // RenderPriority << 20 | additional sort id << 12 | material id
};
static_assert(sizeof(RawOp) == 0x18);
bool sameBytes(const RawOp& a, const RawOp& b);

// The engine's comparators, re-written (disassembled 0x66e670 / 0x66e6a0 / 0x66e600; B-frustum-db.md 4). The tap itself
// sorts with the engine's functions; these document the rules and are unit-tested.
int compareZ(const RawOp& a, const RawOp& b);      // key ascending (front to back)
int compareSolid(const RawOp& a, const RawOp& b);  // u64 (sortId << 32 | key) ascending
int compareAlpha(const RawOp& a, const RawOp& b);  // priority >= 1000 bands first (higher first), key descending, sortId

// why an op issued no draw / several draws (bits; Attribution::end, documented in the N5.0 result)
enum OpNote : std::uint16_t {
    NoteEmptySlot = 1u << 0,      // the slot Execute calls is an empty function (`ret`): e.g. a skinned op in the refracted
                                  // list, the sky's alpha op in the late list
    NoteNotEntered = 1u << 1,     // the engine never called the op's slot (a mismatch: the tap patches every slot it needs)
    NoteInstanced = 1u << 2,      // a mesh's instanced op (desc 0xffffff00 | i): every instance of the resource, in batches
    NoteTreeBatch = 1u << 3,      // a SpeedTree op: all trees of the resource for one geometry type, batches + leaf LODs
    NoteLightPasses = 1u << 4,    // additive light passes after the first (lights in passes of up to 3)
    NoteWholeMesh = 1u << 5,      // a z op of the whole mesh (desc 0xff): consecutive non-masked sub-meshes merged, each
                                  // masked sub-mesh alone
    NoteSkyParts = 1u << 6,       // the sky dome op (dome, clouds, stars, sun, moon)
    NoteZeroGeometry = 1u << 7,   // 0 draws from a non-empty slot (resource not cached / nothing visible in the batch)
    NoteHierarchy = 1u << 8,      // a node op of a mesh hierarchy (desc node << 8 | i, node != 0)
    NoteBillboardPair = 1u << 9,  // a SpeedTree billboard op: every pass twice with two vertex shaders (z: RenderZPass(false)
                                  // + RenderZPass(true); solid: TransformationType 11 duplicates each pass, VS variant 1 / 2)
    NoteGrassBatches = 1u << 10,  // a vegetation op: one instanced draw per run of visible grid nodes with the same
                                  // illumination / light flags (and per 1000 instances of the shared buffer)
    NoteZFill = 1u << 11,         // a draw without colour writes first (EnableAlphaZWrite: the z-fill pass of a blended material)
    NoteNothingToDraw = 1u << 12, // the context's own condition says nothing is drawn (strip < 2 segments, precipitation off,
                                  // particle emitter without live particles), read from the engine at Execute's end
    NoteTreeLod = 1u << 13,       // a SpeedTree op without a draw: every queued tree is at a LOD without geometry of the op's
                                  // type (RenderFronds 0x6f1b0f: a batch whose strip length is 0 is not drawn)
    NoteNoQuads = 1u << 14,       // a particle op with live particles but no draw: eCParticle_PS::UpdateVertices returns
                                  // `primitives != 0` (0x5d6e13): the emitter built no quad this tick
    NoteOverlay = 1u << 15,       // the entity's overlay flags (+0x28: 0x400000 burned, 0x200000 frozen, 0x800000 highlighted)
                                  // add the overlay materials' passes (eCShaderBase::OnGetPasses)
};
// the notes as text ("instanced+light passes"), "-" for none
std::string noteNames(std::uint16_t notes);

// one op of the frame
struct Op {
    RawOp raw;
    const void* vtable = nullptr;
    const void* entity = nullptr;    // context +0x10
    const void* material = nullptr;  // mesh ops: the sub-mesh's eCMaterialResource2 (null: whole mesh / other kinds)
    const void* shader = nullptr;    // its eCShaderBase (+0x70)
    const void* subMesh = nullptr;   // mesh ops: the eCSubMesh the engine will draw (resource +0x58 table, stride 0x48)
    OpKind kind = OpKind::Unknown;
    List list = List::ZPass;
    bool emptySlot = false;          // the slot Execute calls for this list is an empty function
    std::uint32_t index = 0;         // position in its list (execution order)
    // attribution (Attribution)
    std::uint32_t entered = 0;       // times the engine called the op's slot
    std::uint32_t draws = 0;
    std::uint32_t firstDraw = 0;     // device draw number of its first draw
    std::uint32_t instancedDraws = 0, additiveDraws = 0;
    std::uint32_t noColorDraws = 0;  // draws without colour writes
    std::uint32_t otherVsDraws = 0;  // draws whose vertex shader differs from the op's first draw's
    std::uint64_t firstVs = 0;
    struct Brief {
        std::uint64_t vs = 0, ps = 0;
        std::uint32_t blend = 0;
        bool colorWrite = true;
    };
    std::array<Brief, 3> brief{};    // the op's first draws (diagnostics of unexplained multi-draw ops)
    std::int32_t state = -1;         // the context's draw condition at Execute's end (strip: segments PS+0xe8; precipitation:
                                     // PS+0x18 & 1 ? 1 + drops alive (PS+0xe0 - PS+0xc8) : 0; particle: live particles PS+0x390;
                                     // tree billboard: its resource cached at Execute's entry 1 / 0; mesh: bits 1 resource
                                     // cached, 2 visual, 4 materials cached, 8 shaders, at Execute's entry); -1 = not read
    std::uint32_t overlay = 0;       // mesh / skinned ops: the entity's overlay flags (+0x28 & 0xe00000)
    std::uint16_t notes = 0;         // OpNote, set by Attribution::end for ops with 0 or > 1 draws
};
// does the op's draw condition (Op::state) say the engine draws nothing?
bool nothingToDraw(const Op& op);
// ... and the condition is a hard one (a draw would contradict it)
bool cannotDraw(const Op& op);

// the ops of one main-view Execute
struct FrameDesc {
    std::uint64_t frame = 0;        // the device's present count when it was taken
    std::uint32_t view = 0;         // n-th main view of the frame (1 = the first)
    const void* db = nullptr;       // eCFrustumDatabase*
    bool zPrepass = true;           // caps 0x1590: Execute runs the z list
    bool valid = false;
    std::array<const std::uint8_t*, kListCount> liveData{};  // the engine's arrays at Execute's entry
    std::array<std::uint32_t, kListCount> count{}, first{};  // list l = ops[first[l] .. first[l] + count[l])
    std::vector<Op> ops;                                     // all lists in Execute's order
    // the refraction copy (Execute after the +0x1530 list: EndMaterial, CopyCurrentRTToImage of the current target into the
    // render-target desc's refraction image desc +0, the material admin's s1 := it, BeginMaterial), as data
    struct Refraction {
        const void* image = nullptr;  // desc +0: the refraction map's eCImageData (null: Execute copies nothing)
        const void* api = nullptr;    // its API texture (image +0x60): the copy's target
        std::uint32_t copies = 0;     // copies into `api` during the view
        std::uint32_t inPlace = 0;    // ... of those at the documented point (refractionCopyInPlace)
        std::uint32_t other = 0;      // copies into another texture during the view
    } refraction;

    void clear();
    // the op the engine passes to a context slot (a pointer into a list's array) -> the op, null when not in the snapshot
    Op* find(const void* opPointer);
    const Op* find(const void* opPointer) const { return const_cast<FrameDesc*>(this)->find(opPointer); }
    std::size_t total() const { return ops.size(); }
};

// a HAL draw of the main view as the tap sees it
struct DrawInfo {
    std::uint32_t index = 0;  // device draw number
    d3d9::scene::Pass pass = d3d9::scene::Pass::None;
    d3d9::draws::Kind kind = d3d9::draws::Kind::Unknown;
    const void* op = nullptr;  // draws::Tag::op: the op whose slot is executing (null: no op)
    const void* entity = nullptr;
    const void* subMesh = nullptr;
    const void* material = nullptr;
    std::uint32_t instances = 1;
    bool instanced = false;  // stream 0 frequency INDEXEDDATA
    bool additive = false;   // blending with destination ONE (an additive light pass)
    bool colorWrite = true;  // colour writes on
    std::uint64_t vsHash = 0;
    std::uint64_t psHash = 0;
    std::uint32_t blend = 0;  // blend on | src << 8 | dst << 16 | z func << 24
};

// counts of one main view (or summed over a report window)
struct TapCounts {
    std::uint32_t views = 0;
    std::array<std::uint32_t, kListCount> ops{};
    std::uint32_t opsTotal = 0, entered = 0;
    std::uint32_t notEntered = 0;       // ops whose slot was never called
    std::uint32_t outOfOrder = 0;       // op entries that were not the next op of the snapshot's sequence
    std::uint32_t repeated = 0;         // an op entered more than once
    std::uint32_t contentMismatch = 0;  // the op's 0x18 bytes at entry differ from the sorted copy at that position
    std::uint32_t unknownOps = 0;       // slot entries during the view with an op pointer outside the snapshot
    std::uint32_t countMismatch = 0;    // lists whose count at Execute's end differs from the snapshot
    std::array<std::uint32_t, static_cast<std::size_t>(d3d9::scene::Pass::Count)> drawsByPass{};
    std::uint32_t draws = 0, matched = 0;
    std::uint32_t drawOrder = 0;        // a draw whose op is not the op being executed
    std::uint32_t passMismatch = 0;     // a matched draw in another pass than its list's
    std::uint32_t entityMismatch = 0, materialMismatch = 0, subMeshMismatch = 0;
    std::uint32_t unknownOpDraws = 0;   // tagged with an op the snapshot does not have
    std::uint32_t stateContradiction = 0;  // an op that drew although its draw condition says nothing is drawn
    std::array<std::uint32_t, static_cast<std::size_t>(d3d9::draws::Kind::Count)> noOp{};  // draws without an op, by kind
    std::uint32_t noOpTotal = 0;
    // the refraction copy: views with a refraction image, of those copied once at the documented point into it, views where
    // that does not hold (a missing, misplaced or extra copy)
    std::uint32_t refractionViews = 0, refractionOk = 0, refractionBad = 0;
    // per list x kind: ops, draws, ops with 0 draws, ops with > 1 draws, the largest draw count of an op
    struct Bucket {
        std::uint32_t ops = 0, draws = 0, zero = 0, multi = 0, maxDraws = 0;
        // 0 draws: empty slot / nothing to draw (the engine's condition read) / by a decompiled rule (not checked at run time) / other
        std::uint32_t zeroEmpty = 0, zeroNothing = 0, zeroRule = 0, zeroOther = 0;
        std::uint32_t multiInstanced = 0, multiTree = 0, multiLights = 0, multiWholeMesh = 0, multiSky = 0, multiBillboard = 0,
                      multiGrass = 0, multiZFill = 0, multiOverlay = 0, multiOther = 0;
    };
    std::array<std::array<Bucket, kOpKindCount>, kListCount> buckets{};

    void add(const TapCounts& o);
    // true when every check holds: every op entered once and in order with the snapshot's bytes, every draw matched or
    // explained (ShadowMask / Decal / Billboard), no unexplained 0- or multi-draw op
    bool clean() const;
};

// The attribution of one main view: begin(), then the engine's slot entries and the view's draws as they happen, end()
class Attribution {
public:
    void begin(FrameDesc* frame);
    bool active() const { return frame_ != nullptr; }
    // the engine called a context slot with `opPointer`; `live` = the op's bytes at that moment (null: unreadable)
    void enterOp(const void* opPointer, const RawOp* live);
    void draw(const DrawInfo& d);
    // the view's Execute returned; liveCounts = the lists' counts at that moment
    void end(const std::array<std::uint32_t, kListCount>& liveCounts);
    const TapCounts& counts() const { return counts_; }
    // the global index of the op expected next (= every op before it entered)
    std::size_t nextOp() const { return next_; }
    // the first mismatches of this view, named (kind, list, index, entity, desc)
    const std::vector<std::string>& mismatches() const { return mismatches_; }

private:
    void note(std::string text);
    FrameDesc* frame_ = nullptr;
    std::size_t next_ = 0;            // global index of the op expected next
    const Op* current_ = nullptr;     // the op last entered
    TapCounts counts_;
    std::vector<std::string> mismatches_;
};

// is a refraction copy, made when the op expected next is `nextOp`, at the place Execute makes it: after the last op up to
// the +0x1530 list (sky dome), before the first refracted op
bool refractionCopyInPlace(const FrameDesc& f, std::size_t nextOp);

// is a draw without an op one of the documented non-op paths of the main view (the screen-space shadow mask, decals,
// opaque billboards of the billboard admin)?
bool explainedWithoutOp(d3d9::draws::Kind kind);

// "Op [Solid #12] Mesh ctx .. entity .. desc 0x.. key 0x.. sort 0x.. fade 1.000 -> 2 draws (#345) instanced+..." (-LogFrame)
std::string describe(const Op& op);
// the report lines (rvk.log `native: frame tap: ...`)
std::vector<std::string> reportLines(const TapCounts& last, std::uint64_t lastFrame, const TapCounts& window,
                                     const std::vector<std::string>& mismatches);

}  // namespace rvk::native
