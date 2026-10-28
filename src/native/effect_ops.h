#pragma once

// N5.5c "effect draws" (docs/research/engine-frame/README.md "13. N5.5c result"; native.scene_ops / native.scene_ops_compare,
// group bit 1024 of native.scene_ops_groups): the draws of the engine's image filters around the shadows, composed from engine
// data and compared with (compare) / drawn instead of (take-over) the engine's:
//  * the post_SSM shadow mask chain of eCIlluminationAdmin::UpdateShadowMask (illum +0x430 SSAO, +0x438 / +0x440 BlurSSAO, +0x448
//    eCImageFilterShadowMask: UpscaleSSAO, one SSM / DSSM quad per sun cascade, VSSM; +0x458 Blur), each filter's
//    eCImageFilterEffect::ExecuteEffect: the parameter objects' values (the mask's own per cascade from the cascade object and
//    the application camera, the semantic ones from their sources, the rest as stored), the inputs' output images, the fx pass
//    (states, preshaders, samplers: native/effect_compose), the visual's states and quad, the filter's target / depth / viewport;
//  * the 1/4 depth copy (illum +0x450, eCImageFilterCopy: CopyImageToCurrentRT of the depth map into its R32F target);
//  * the terrain variance map's post_blur Blur7H / Blur7V (eCShadowMapDirectionalLight2VS::RenderShadowMap 0x5a1b40, the map's
//    blur filter +0x110: map -> temp +0x108 -> map).
// N5.7 (README.md "15. N5.7 result"; group 32768): the post chain: every effect filter job of the image-effect graphs
// eCImageEffect::Execute runs (the scene effect's jobs after the scene: luminance, bloom, streaks, tone map, DoF; the overlay
// effect), composed the same way (semantic parameters natively from the timer, weather, image filter admin, camera and sky dome;
// the inputs through the graph's links, incl. the scene filter's outputs and nested effects; texture-mode targets with the depth
// bound at Execute's entry, screen-mode filters into what is bound). effect_ops_post.cpp.
// N5.7 (group 65536): the sprite batcher's draws (eCSprite::Flush: GUI, text, cursor, videos): the batches of RenderBatches
// re-done from the queued records (blend / texture / buffer-full breaks, SetBlend's states, the sprite effect's pass with the
// world-view-projection composed, the mesh's ranges) with the vertex data generated natively; take-over draws from our own
// vertex buffer. effect_ops_gui.cpp.
// Take-over: the composed draws are issued at ExecuteEffect's entry (the device state saved and restored around them) and the
// engine's draws of that call are dropped at prepareDraw; the copy is made by us at the HAL call and the engine's is skipped.
// The engine's code still runs completely (parameter objects, effect state, render-target pool, the stencil it relies on is
// written by our draws as by its own). Off: nothing beyond the detours' null check. Render thread.

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9/device.h"
#include "engine/scene_hooks.h"
#include "native/effect_compose.h"

namespace rvk::hal::vulkan {
class HalDevice;
class EffectInfo;
}  // namespace rvk::hal::vulkan

namespace rvk::native {

class EffectOps final : public engine::scene::EffectTap {
public:
    explicit EffectOps(hal::vulkan::HalDevice& hal);
    ~EffectOps() override;
    EffectOps(const EffectOps&) = delete;
    EffectOps& operator=(const EffectOps&) = delete;

    void setMode(bool takeOver, bool compare);
    // group 2048: the shadow views' setup (native/effect_ops_setup.cpp)
    void setSetupMode(bool takeOver, bool compare);
    bool maskOn() const { return takeOver_ || compare_; }
    bool setupOn() const { return setupTake_ || setupCompare_; }
    // N5.7: group 32768 the post chain, group 65536 the sprite batcher
    void setPostMode(bool takeOver, bool compare);
    void setGuiMode(bool takeOver, bool compare);
    bool postOn() const { return postTake_ || postCompare_; }
    bool guiOn() const { return guiTake_ || guiCompare_; }
    bool active() const { return maskOn() || setupOn() || postOn() || guiOn(); }

    // engine::scene::EffectTap
    void maskBegin(void* illum) override;
    void maskEnd() override;
    void effectBegin(void* filter) override;
    void effectEnd(void* filter) override;
    void copyBegin(void* filter) override;
    void copyEnd(void* filter) override;
    void shadowMapBegin(void* map) override;
    void shadowMapEnd(void* map) override;
    void shadowSceneBegin(void* map) override;
    void shadowSceneEnd(void* map) override;
    void postBegin(void* effect) override;
    void postEnd(void* effect) override;
    void spriteFlushBegin(void* sprite) override;
    void spriteFlushEnd(void* sprite) override;
    // engine/shadow_maps: eCCascadeShadowMap::RenderShadowMaps is about to fit and render the cascades
    void fitBegin(void* cascade);
    // the HAL's device calls inside a shadow map's setup / reset: compared with (and with take-over replaced by) the composition
    void setupTarget(void*& rt, void*& depth);
    void setupViewport(fxc::Viewport& vp);
    void setupClear(std::uint32_t& flags, std::uint32_t& colour, float& z, std::uint32_t& stencil);

    // the HAL's CopyImageToCurrentRT (HalTexture: `src` = its texture, the engine's rects and filter); true = skip the engine's copy
    bool copyCall(d3d9::Texture* src, const std::int32_t* srcRect, const std::int32_t* dstRect, std::uint32_t filter);

    // draw routing (native/frame_tap): our own draws / an engine draw (true: drop it)
    bool issuing() const { return issuing_; }
    void ownDraw(const d3d9::DrawTapInfo& draw);
    bool engineDraw(const d3d9::DrawTapInfo& draw);
    // N5.8 coverage: why an engine draw that was not dropped stays the engine's (empty: no unit of ours claims it)
    std::string_view leftReason(const d3d9::DrawTapInfo& draw) const;
    void report();

private:
    enum class Unit : std::uint8_t { None, Chain, Mask, TerrainBlur, Copy, Post, Gui };
    enum class Skip : std::uint8_t {
        None, NoEffect, Technique, ParamClass, Semantic, Input, Image, Visual, Packed, Unreadable, Buffer, Sorted, Count
    };
    static const char* name(Skip s);
    struct Live {
        std::bitset<fxc::kVsRegs> vs;
        std::bitset<fxc::kPsRegs> ps;
    };
    // a parameter value the unit's engine code writes before the draw (the mask's own parameters): bytes or an image
    struct Override {
        std::vector<std::uint8_t> bytes;
        const void* image = nullptr;
        bool isImage = false;
    };

    bool verify();
    const Live& liveOf(const d3d9::Shader* s);
    d3d9::Texture* textureOfImage(const void* imageData) const;
    d3d9::Surface* surfaceOfImage(const void* imageData) const;
    // the image a source filter's output `index` is (eCImageFilterRTBase +0x70, eCImageFilterTexture by mode); false: unknown class
    bool outputImage(const std::uint8_t* filter, std::int32_t index, const void*& image) const;
    const std::uint8_t* camera() const;
    // the mask's step `step` of its plan: its technique and the parameters it has written up to it (into overrides_)
    bool maskStep(std::size_t step, std::string& technique);
    // the draws of one ExecuteEffect: technique by name (empty: the filter's +0xf0), the inputs' images (override: input 0)
    Skip composeEffect(const std::uint8_t* filter, const std::string& technique, const void* input0, const void* target, bool screenTarget);
    void captureDevice(fxc::EffectDraw& out) const;
    void capture(const d3d9::DrawTapInfo& t, fxc::EffectDraw& out, fxc::Live& live);
    // sets the device to `d` (only what differs)
    void applyDevice(const fxc::EffectDraw& d);
    void issue();
    void finishUnit();
    void compareDraw(const fxc::EffectDraw& composed, const d3d9::DrawTapInfo& t, bool own);

    // ---- N5.7 post chain (effect_ops_post.cpp) ----
    bool verifyPost();
    // a parameter object of a semantic class: its value as the class's Set (+0x28) computes it; `set` false = the class sets
    // nothing this call (the effect keeps its last value)
    Skip semanticValue(const std::uint8_t* obj, std::uint32_t vtRva, const fx::Parameter& param, std::vector<std::uint8_t>& out, bool& set);
    // the image of output `index` of any filter class of the graphs (scene, effect / copy (RTBase), texture, image, nested effect)
    bool graphOutput(const std::uint8_t* filter, std::int32_t index, const void*& image, int depth = 0) const;
    std::int32_t outputCount(const std::uint8_t* filter, int depth = 0) const;
    const std::uint8_t* nestedEffect(const std::uint8_t* imageEffectFilter) const;
    std::string graphName(const std::uint8_t* effect) const;
    // eCGfxEffect::Execute 0x750210's skip (ms_bOptimize, the active effect, the object's dirty byte +0x2c, ms_bReset)
    bool executeSkipped(const std::uint8_t* gfxEffect) const;
    void reportPost();
    bool postTake_ = false, postCompare_ = false;
    int postVerified_ = 0;
    int postDepth_ = 0;
    const std::uint8_t* postEffect_ = nullptr;   // the eCImageEffect of the current Execute
    std::string postGraph_;                      // "scene" / "overlay" / "other" + its filters' names
    d3d9::Surface* postRt_ = nullptr;            // the target / depth / viewport bound at Execute's entry
    d3d9::Surface* postDepthSurface_ = nullptr;
    fxc::Viewport postViewport_;
    std::map<const void*, std::string> graphsSeen_;
    std::map<std::string, std::uint64_t> graphRuns_, graphRunsTotal_;
    std::uint64_t postDrawsOutside_ = 0;         // engine draws of a Post view outside the composed units
    void* (*filterAdmin_)() = nullptr;           // FUN_180095750 (eCImageFilterAdmin)
    void* (*weatherAdmin_)() = nullptr;          // FUN_180275c30 (eCWeatherAdmin)
    void* (*illumAdmin_)() = nullptr;            // FUN_18014f330 (eCIlluminationAdmin)
    void* (*effectMapFind_)(void* map, const void* name) = nullptr;  // FUN_1801f2410 (the admin's effect map +0x68 by name)
    void* (*getSkydome_)(void* illum) = nullptr;
    void* (*timer_)() = nullptr;
    float thousand_ = 1000.0f, minusOne_ = -1.0f;
    float lumWeights_[3]{0.3f, 0.59f, 0.11f};
    float overlayWeight_ = 0.0f;
    std::uint64_t postCopies_ = 0;
    const float* epsilon_ = nullptr;
    const void* vtScene_ = nullptr;
    const void* vtImageEffect_ = nullptr;
    const void* vtImage_ = nullptr;
    using VecMatFn2 = void (*)(void* v, const void* m);
    VecMatFn2 vecTransformDirection_ = nullptr, vecMulAssign_ = nullptr;
    using MatMulFn = void* (*)(const void* a, void* out, const void* b);
    MatMulFn matMul_ = nullptr;

    // ---- N5.7 sprite batcher (effect_ops_gui.cpp) ----
    bool verifyGui();
    Skip composeSprite(const std::uint8_t* sprite);
    bool spriteGeometryEqual(std::size_t k, const d3d9::DrawTapInfo& t, bool own, std::string& why);
    void issueSprite();
    void reportGui();
    bool guiTake_ = false, guiCompare_ = false;
    int guiVerified_ = 0;
    const std::uint8_t* spriteFlush_ = nullptr;  // the eCSprite of the current Flush
    struct SpriteGeom {
        std::uint32_t vbOffset = 0;              // bytes into the sprite mesh's vertex buffer
        std::vector<std::uint8_t> vertices;      // count x 4 x 0x1c (the w lane of each position is not compared: decl FLOAT3)
        std::vector<std::uint8_t> indices;       // count x 6 x 2 at the startIndex
    };
    std::vector<SpriteGeom> spriteGeom_;
    std::unique_ptr<d3d9::Buffer> spriteVb_;     // take-over: our own vertex buffer (the engine's offsets)
    std::unique_ptr<d3d9::Buffer> spriteIb_;     // ... and index buffer (the static quad indices, generated)
    std::uint64_t spriteFlushes_ = 0, spriteRecords_ = 0, spriteQuads_ = 0, spriteBreaksBlend_ = 0, spriteBreaksTexture_ = 0,
                  spriteBreaksFull_ = 0, spriteSkippedExecute_ = 0, spriteWLaneDiffers_ = 0, guiOutside_ = 0,
                  spriteNullTechnique_ = 0;
    std::uint32_t spriteLogged_ = 0;
    bool loggedOptimize_ = false;

    // ---- the shadow views' setup (group 2048) ----
    enum class SetupField : std::uint8_t { Fit, Camera, TextureMatrix, Target, Viewport, Clear, Restore, Calls, Count };
    static const char* name(SetupField f);
    struct SetupStats {
        std::uint64_t units = 0, composed = 0, calls = 0, cleanCalls = 0, cameras = 0, cleanCameras = 0, fits = 0, cleanFits = 0, taken = 0;
        std::array<std::uint64_t, static_cast<std::size_t>(SetupField::Count)> mismatches{};
        std::uint64_t skipped = 0;
        void add(const SetupStats& o);
    };
    struct Fit {
        bool valid = false;
        float start = 0.0f;
        float light[16]{};
        float box[8]{};
        std::uint8_t frustum[0x60]{};
    };
    bool verifySetup();
    void installSetupTap();
    void reportSetup();
    void setupMiss(SetupField f, const std::string& what);
    void composeCamera(const float light[16], const float box[6], const std::uint8_t* map);
    bool setupTake_ = false, setupCompare_ = false;
    int setupVerified_ = 0;
    std::array<Fit, 4> fit_{};
    const void* fitCascade_ = nullptr;
    std::uint64_t fitFrame_ = ~0ull;
    // the unit: one eCShadowMapBase::RenderShadowMap of a cascade / the terrain map
    const std::uint8_t* setupMap_ = nullptr;
    std::string setupKind_;
    SetupStats* setupStats_ = nullptr;
    int setupStep_ = 0;    // 0 target, 1 viewport, 2 clear, 3 the scene ran, 4 restore target, 5 restore viewport, 6 done
    bool inScene_ = false;
    bool restoreTarget_ = false;
    d3d9::Surface* setupRt_ = nullptr;
    d3d9::Surface* setupDepth_ = nullptr;
    fxc::Viewport setupViewport_;
    std::uint32_t setupClearFlags_ = 0, setupClearColour_ = 0;
    d3d9::Surface* entryRt_ = nullptr;
    d3d9::Surface* entryDepth_ = nullptr;
    fxc::Viewport entryViewport_;
    float camView_[16]{}, camProj_[16]{}, texMatrix_[16]{};
    float camFar_ = 0.0f;
    bool setupComposed_ = false;
    std::map<std::string, SetupStats> setupWindow_, setupTotal_;
    std::uint32_t setupLogged_ = 0;
    // SharedBase / Engine functions of the fit and UpdateCamera
    using VecMatFn = void* (*)(void* v, const void* m);
    using VecFn = const void* (*)(void* v);
    using LookAtFn = void (*)(void* m, const void* dir);
    using BoxMinMaxFn = void (*)(void* box, const void* min, const void* max);
    using BoxCentreFn = void (*)(void* box, const void* c, float r);
    using BoxCopyFn = void (*)(void* box, const void* other);
    using FrustumBoxFn = void* (*)(void* f, const void* box);
    using FrustumMatFn = void (*)(void* f, const void* m);
    using FrustumFn = void (*)(void* f);
    using VertexFn = void* (*)(const void* f, void* out, int i);
    using SphereFn = void (*)(void* s);
    using SphereMergeFn = void (*)(void* s, const void* v);
    using ScaleVecFn = void (*)(void* m, const void* v);
    using FloatFn = float (*)(float, float);
    VecMatFn vecMul_ = nullptr;
    VecFn vecNormalize_ = nullptr;
    LookAtFn lookAt_ = nullptr;
    BoxMinMaxFn boxMinMax_ = nullptr;
    BoxCentreFn boxCentre_ = nullptr;
    BoxCopyFn boxCopy_ = nullptr;
    FrustumBoxFn frustumFromBox_ = nullptr;
    FrustumMatFn frustumInvTransform_ = nullptr, frustumTransform_ = nullptr;
    FrustumFn frustumNormalize_ = nullptr;
    VertexFn frustumVertex_ = nullptr;
    SphereFn sphereInvalidate_ = nullptr;
    SphereMergeFn sphereMerge_ = nullptr;
    ScaleVecFn matScaleVec_ = nullptr;
    FloatFn powf_ = nullptr, fmodf_ = nullptr;

    hal::vulkan::HalDevice& hal_;
    d3d9::Device& dev_;
    bool takeOver_ = false, compare_ = false;
    bool unitTake_ = false, unitCompare_ = false;  // the current unit's group switches (mask, post or GUI)
    int verified_ = 0;
    const std::uint8_t* engine_ = nullptr;
    // engine data (Engine.dll): constants of the mask's OnUpdateOutput, the classes' vtables
    fxc::MaskConstants k_;
    float one_ = 1.0f;
    const void* vtEffect_ = nullptr;
    const void* vtMask_ = nullptr;
    const void* vtCopy_ = nullptr;
    const void* vtTexture_ = nullptr;
    const void* vtTextureRandom_ = nullptr;
    void* (*materialAdmin_)() = nullptr;
    using MatFn = void (*)(void* m);
    using MatMulAssignFn = void* (*)(void* a, const void* b);
    using MatScaleFn = void (*)(void* m, float s);
    MatFn matInvert_ = nullptr, matIdentity_ = nullptr;
    MatMulAssignFn matMulAssign_ = nullptr;
    MatScaleFn matScale_ = nullptr;

    // the mask unit (UpdateShadowMask)
    const std::uint8_t* illum_ = nullptr;  // the illumination admin of the last UpdateShadowMask
    bool inMask_ = false;
    d3d9::Surface* maskDepth_ = nullptr;   // the depth surface bound at UpdateShadowMask's entry (every filter keeps it)
    std::vector<std::string> plan_;        // the mask filter's ExecuteEffect calls in order (techniques)
    std::vector<std::int32_t> planCascade_;
    std::size_t planNext_ = 0;
    std::map<std::string, Override> overrides_;
    float r_[16]{};                        // depth -> view space -> world (inv(V) x inv(diag(P11, P22, 1)) x far)
    std::uint64_t blurFrame_ = ~0ull;
    std::uint32_t blurIndex_ = 0;

    // the current unit
    Unit unit_ = Unit::None;
    const void* unitFilter_ = nullptr;
    std::string unitName_;
    std::vector<std::unique_ptr<fxc::EffectDraw>> draws_;
    std::size_t drawCount_ = 0;
    std::uint32_t engineDraws_ = 0;
    bool taken_ = false;
    bool issuing_ = false;
    std::size_t issueIndex_ = 0;
    std::vector<std::uint8_t> ownKind_;
    std::unique_ptr<fxc::EffectDraw> scratch_, saved_;
    // the copy unit
    struct CopyDesc {
        d3d9::Texture* src = nullptr;
        d3d9::Surface* dst = nullptr;
        std::int32_t dstRect[4]{};
        std::uint32_t filter = 1;
        fxc::Viewport viewport;
        bool valid = false;
    } copy_;
    std::uint32_t copyCalls_ = 0;

    // counts per unit name (technique / "copy"), window and total
    struct Stats {
        fxc::Counts counts;
        std::array<std::uint64_t, static_cast<std::size_t>(Skip::Count)> skips{};
        std::uint64_t issuedDraws = 0, droppedDraws = 0, ownChecked = 0, ownMismatch = 0;
        void add(const Stats& o);
    };
    std::map<std::string, Stats> window_, total_;
    Stats* stats_ = nullptr;
    std::uint64_t outsideDraws_ = 0, outsideDrawsTotal_ = 0;  // engine draws inside UpdateShadowMask outside any unit (PSSM)
    std::uint64_t unusedParams_ = 0, masks_ = 0, packedMasks_ = 0, nativeParams_ = 0;
    std::uint32_t mismatchLogged_ = 0, paramClassLogged_ = 0, packedLogged_ = 0;
    std::vector<std::string> firstMismatches_;
    std::unordered_map<const d3d9::Shader*, Live> live_;
    // N5.8 coverage: the unit left to the engine (its filter / sprite until its end) and why
    const void* leftUnit_ = nullptr;
    std::string leftWhy_;
    void leftUnit(const void* filter, Skip s);
};

}  // namespace rvk::native
