// The owned stages' state line (docs/research/frame-ownership.md 7.47): one line per 5 s window that names, for every stage of the frame-ownership
// family, whether its SETTING is on, whether the code is armed, and whether it ACTED in the window. The A/B scripts (scripts/lib/stage-state.ps1) read it:
// a toggle that does not apply (7.46: `set native.owned_trees` was never wired to applyStaticOps) shows as `inactive(setting on, code off)`.

#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "core/settings.h"
#include "enh/rt_scene.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/owned_compact.h"
#include "native/f10_request.h"
#include "native/f10_extract.h"
#include "native/own_post.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

// (S-9, frame-ownership.md 14.41: the stages default on and act in Full only: outside Full a setting counts as off; the start-time ones (clusters) only
// in a session started in Full)
bool settingOn(const settings::Setting* s) { return s != nullptr && s->asBool() && settings::fullNow(); }
// (restart settings: what the session started with is what runs)
bool startOn(const settings::Setting* s) { return s != nullptr && s->startValue() != 0.0 && settings::startedFull(); }
int settingInt(const settings::Setting* s) { return s != nullptr && settings::fullNow() ? static_cast<int>(s->asInt()) : 0; }

// `needs` = the unmet prerequisites (setting keys) that keep the code off; `gate` = why an armed stage owned nothing (the owned path's gate of the window)
std::string stage(const char* name, bool setting, bool armed, std::uint64_t acted, const std::string& detail, const std::string& needs = {}, const std::string& gate = {}) {
    std::string state;
    if (!setting && !armed) state = "inactive(setting off)";
    else if (!setting && armed) state = std::format("INCONSISTENT(setting off, code on, acted {})", acted);
    else if (!armed) state = needs.empty() ? "inactive(setting on, code off)" : std::format("inactive(setting on, code off: needs {})", needs);
    else if (acted == 0) state = gate.empty() ? "inactive(armed, nothing owned in the window)" : std::format("inactive(armed, gated: {})", gate);
    else state = std::format("active({})", detail);
    return std::format(" {}={}", name, state);
}

// the unmet prerequisites of a stage: the setting's declared needs (Spec::needs) + the settings the code arms it with (hal/vulkan/device.cpp applyStaticOps), off ones by key
std::string needsOf(const settings::Setting* s, std::initializer_list<const settings::Setting*> code) {
    std::string out = s != nullptr ? settings::registry().unmetNeedKeys(*s) : std::string{};
    for (const settings::Setting* c : code) {
        if (c == nullptr || c->asBool()) continue;
        const std::string k = c->spec().key;
        if (out.find(k) != std::string::npos) continue;
        out += std::format("{}{}", out.empty() ? "" : ", ", k);
    }
    return out;
}

}  // namespace

void StaticOps::stagesReport() {
    const auto& rs = settings::renderer();
    static std::uint64_t lastCullJobs = 0;
    gpuscene::OwnedCompact::Stats& cs = gpuscene::ownedcompact().stats();
    const std::uint64_t cullJobs = cs.cullJobs - lastCullJobs;
    lastCullJobs = cs.cullJobs;
    std::string line;
    // the owned path's gate of the window (main views; the cascades' own for owned_shadows): the stages behind it name it while they own nothing
    const std::string gateMain = ownedGateText(ownedW_), gateCasc = ownedGateText(ownedShadowW_);
    const std::string gateAny = !gateMain.empty() ? gateMain : gateCasc;
    const auto own = [&](const std::string& own) { return !own.empty() ? own : gateMain; };
    // (the code prerequisites of applyStaticOps, by setting)
    const std::initializer_list<const settings::Setting*> base = {rs.nativeSceneDraws, rs.nativeOwnedStatic};
    line += stage("owned_static", settingOn(rs.nativeOwnedStatic), ownedStatic_, ownedW_.owned, std::format("{} ops owned in {} views", ownedW_.owned, ownedW_.views),
                  needsOf(rs.nativeOwnedStatic, {rs.nativeSceneDraws}), gateMain);
    line += stage("owned_persistent", settingOn(rs.nativeOwnedPersistent), ownedPersistent_, persistW_.passes, std::format("{} passes", persistW_.passes), needsOf(rs.nativeOwnedPersistent, base), gateMain);
    const std::uint64_t treeMain = tro_ ? tro_->window.ownedViews : 0, treeCasc = tro_ ? tro_->shadowWindow.ownedViews : 0;
    line += stage("owned_trees", settingOn(rs.nativeOwnedTrees), ownedTrees_, treeMain + treeCasc, std::format("main {} owned views, cascades {}", treeMain, treeCasc),
                  needsOf(rs.nativeOwnedTrees, {rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}), own(tro_ ? tro_->gate : std::string{}));
    line += stage("owned_shadows", settingOn(rs.nativeOwnedShadows), ownedShadowsOn(), ownedShadowW_.owned, std::format("{} cascade ops owned", ownedShadowW_.owned),
                  ownedShadowsTripped_ ? std::string("a reset (tripped by a MISS / DIFFERING)") : needsOf(rs.nativeOwnedShadows, {rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}),
                  !gateCasc.empty() ? gateCasc : gateMain);
    // F3-4 (native.owned_shadow_views): acted = the terrain map's + the atlas cells' ops drawn from records (meshes / actors) + their tree ops taken in the window
    {
        const std::uint64_t mesh = ownedLightW_[0].owned + ownedLightW_[1].owned, trees = tro_ ? tro_->lightWindow.opsTaken : 0;
        const std::string gateLight = ownedGateText(ownedLightW_[1]).empty() ? ownedGateText(ownedLightW_[0]) : ownedGateText(ownedLightW_[1]);
        line += stage("owned_shadow_views", settingOn(rs.nativeOwnedShadowViews), ownedShadowViewsOn(), mesh + trees,
                      std::format("terrain map {} ops, atlas cells {} ops, their trees {} ops", ownedLightW_[0].owned, ownedLightW_[1].owned, trees),
                      ownedShadowsTripped_ ? std::string("a reset (tripped by a MISS / DIFFERING)") : needsOf(rs.nativeOwnedShadowViews, {rs.nativeOwnedShadows, rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}),
                      !gateLight.empty() ? gateLight : gateMain);
    }
    line += stage("owned_actors", settingOn(rs.nativeOwnedActors), ownedActorsOn(), actorW_.drawn, std::format("{} draws; F6-2 {}: {} ordered ops, {} morph pick swaps", actorW_.drawn, f62On() ? "on" : "off (RVK_F62_OFF)", actorW_.ordOwned, actorW_.pickSwaps),
                  needsOf(rs.nativeOwnedActors, {rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent, rs.nativeOwnedObjects}), gateAny);
    // F2f (native.owned_instances): acted = render-instance ops drawn from their records in the window
    line += stage("owned_instances", settingOn(rs.nativeOwnedInstances), ownedInstancesOn(), instW_.ownedOps, std::format("{} instance ops / {} draws owned", instW_.ownedOps, instW_.ownedDraws),
                  needsOf(rs.nativeOwnedInstances, {rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}), gateMain);
    instReport();
    // F9 + F14 (native.owned_ordered): acted = order-dependent ops drawn from their records at their place in the window (solid + refracted + alpha lists)
    {
        static std::uint64_t last = 0;
        const std::uint64_t a = ordTotalOwned_ - last;
        last = ordTotalOwned_;
        line += stage("owned_ordered", settingOn(rs.nativeOwnedOrdered), ownedOrderedOn(), a, std::format("{} ordered ops owned", a),
                      needsOf(rs.nativeOwnedOrdered, {rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}), gateMain);
    }
    line += stage("owned_vegetation", settingOn(rs.nativeOwnedVegetation), ownedVeg_, vegStat_.retained, std::format("{} ops on retained streams", vegStat_.retained), needsOf(rs.nativeOwnedVegetation, {}));
    const std::uint64_t vegViews = vro_ ? vro_->window.ownedViews : 0;
    line += stage("owned_vegetation_ops", settingOn(rs.nativeOwnedVegetationOps), ownedVegOps_, vegViews, std::format("{} owned views", vegViews),
                  needsOf(rs.nativeOwnedVegetationOps, {rs.nativeOwnedVegetation, rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}), own(vro_ ? vro_->gate : std::string{}));
    line += stage("clusters", startOn(rs.nativeClusters), gpuscene::clusterstore().active(), gpuscene::clusterstore().active() ? 1 : 0, "store active", needsOf(rs.nativeClusters, {}));
    line += stage("cluster_cull", startOn(rs.nativeClusterCull), dev_.clusterCullOn(), cullJobs, std::format("{} jobs with the cull", cullJobs), needsOf(rs.nativeClusterCull, {rs.nativeClusters}),
                  gateAny);
    // S-6: depth-only z passes (native.depth_z; acted = main z scopes without the depth map attachment + pixel shaders left out in the window)
    static std::uint64_t lastDepthZ = 0;
    const std::uint64_t dz = dev_.depthZActs() - lastDepthZ;
    lastDepthZ = dev_.depthZActs();
    const bool dzSetting = settingInt(rs.nativeDepthZ) != 0;
    line += stage("depth_z", dzSetting, dev_.depthZ() != d3d9::Device::DepthZ::Off, dz, std::format("{} z scopes / pixel shaders without colour output", dz));
    // S-4: vertex pulling (native.vertex_pull; acted = owned items drawn through the pulled vertex shaders in the window)
    static std::uint64_t lastPull = 0;
    const std::uint64_t vp = dev_.vertexPullActs() - lastPull;
    lastPull = dev_.vertexPullActs();
    line += stage("vertex_pull", settingOn(rs.nativeVertexPull), dev_.vertexPull(), vp, std::format("{} items pulled", vp),
                  needsOf(rs.nativeVertexPull, {rs.nativeSceneDraws, rs.nativeOwnedStatic, rs.nativeOwnedPersistent}), gateAny);
    vertexPullReport();
    // S-5: own foliage (native.own_foliage; acted = grass instances shrunk or removed by the distance thinning in the window)
    {
        static std::uint64_t last = 0;
        const std::uint64_t acted = foliageTotal_.changed + foliageW_.changed;
        const std::uint64_t a = acted - last;
        last = acted;
        line += stage("own_foliage", settingOn(rs.nativeOwnFoliage), ownFoliage_, a, std::format("{} grass instances thinned", a));
        foliageReport();
    }
    // F4-5: the RT scene's animated SpeedTrees by compute (native.tree_wind; acted = tree instances evaluated by the compute pass in the window)
    {
        static std::uint64_t lastJobs = 0, lastOwned = 0;
        const enh::TreeWindState tw = enh::treeWindState();
        const std::uint64_t a = tw.jobs - lastJobs, o = tw.owned - lastOwned;
        lastJobs = tw.jobs;
        lastOwned = tw.owned;
        const bool twSetting = settingInt(rs.nativeTreeWind) != 0;
        // (S-9: with no ray-traced consumer of the swaying trees (player defaults: no RT shadows / GI) the pipeline is never requested: armed, idle)
        line += stage("tree_wind", twSetting, tw.armed || (twSetting && !tw.requested), a, std::format("{} tree instances by compute, {} of them from owned runs", a, o));
    }
    // 14.42: the swaying trees' / grass' wind motion (native.tree_motion; acted = composed / replayed draws + owned instances drawn with the wind-motion forms in the window)
    {
        static std::uint64_t last = 0;
        const d3d9::Device::TreeMotionStats& w = dev_.windStats;
        const std::uint64_t acted = w.perKind[0] + w.perKind[1];
        const std::uint64_t a = acted - last;
        last = acted;
        const int want = settingInt(rs.nativeTreeMotion);
        line += stage("tree_motion", want != 0 && settings::fullNow(), dev_.treeMotion() != 0, a, std::format("{} tree / grass draws or owned instances with their wind motion", a));
    }
    // F8-1: the RT scene from the owned record set (native.rt_records; acted = owned draws whose sightings were replayed from their scripts in the window)
    {
        static std::uint64_t last = 0;
        const enh::RtRecordsState st = enh::rtRecordsState();
        const std::uint64_t a = st.replayed - last;
        last = st.replayed;
        line += stage("rt_records", settingOn(rs.nativeRtRecords), st.armed, a, std::format("{} owned draws replayed from their record scripts", a),
                      needsOf(rs.nativeRtRecords, {rs.nativeOwnedPersistent}));
    }
    // frame-ownership.md 14.43: the RT scene's TLAS instances built on the GPU (native.rt_gpu_build; acted = instances the compute pass wrote in the window)
    {
        static std::uint64_t last = 0;
        const enh::RtGpuBuildState st = enh::rtGpuBuildState();
        const std::uint64_t a = st.built - last;
        last = st.built;
        line += stage("rt_gpu_build", settingOn(rs.nativeRtGpuBuild), st.armed, a,
                      std::format("{} TLAS instances written by the compute pass; run: {} check frames compared, DIFFERING {}", a, st.compared, st.differing));
    }
    // F10-1: the main view's static mesh contexts requested natively (native.own_collection; acted = contexts requested by the port in the window)
    {
        static std::uint64_t last = 0, lastDef = 0, lastCmp = 0;
        const f10req::StageState st = f10req::stageState();
        const std::uint64_t a = st.acted - last, d = st.deferred - lastDef, c = st.compared - lastCmp;
        last = st.acted;
        lastDef = st.deferred;
        lastCmp = st.compared;
        line += stage("own_collection", st.setting != 0, st.armed != 0 && st.armed == st.setting, st.setting == 2 ? c : a,
                      st.setting == 2 ? std::format("compare: {} contexts compared; run DIFFERING {}", c, st.differing)
                                      : std::format("on: {} contexts requested natively, {} z / solid ops deferred, {} compared; run DIFFERING {}", a, d, c, st.differing));
    }
    // F10-2: the main view's ExtractFromFrustum done natively (native.own_extract; acted = views extracted natively in the window, compare: views compared)
    {
        static std::uint64_t lastLive = 0, lastCmp = 0;
        const f10x::StageState st = f10x::stageState();
        const std::uint64_t a = st.live - lastLive, c = st.compared - lastCmp;
        lastLive = st.live;
        lastCmp = st.compared;
        line += stage("own_extract", st.setting != 0, st.armed != 0 && st.armed == st.setting, st.setting == 2 ? c : a,
                      std::format("{} views extracted natively, {} compared; run DIFFERING {}", a, c, st.differing));
    }
    // 14.13: bone-history motion (native.bone_motion, restart; acted = skinned solid draws drawn with the bone-motion forms, composed + owned records, in the window)
    {
        static std::uint64_t last = 0;
        const d3d9::Device::BoneMotionStats& b = dev_.boneStats;
        const std::uint64_t acted = b.chosen + b.records + b.compareDraws;
        const std::uint64_t a = acted - last;
        last = acted;
        const bool boneSetting = rs.nativeBoneMotion != nullptr && rs.nativeBoneMotion->startValue() != 0.0 && settings::fullNow();
        line += stage("bone_motion", boneSetting, dev_.boneMotion() != 0, a, std::format("{} skinned solid draws / records", a));
    }
    // S-7 .. S-7d: the post chain, the shadow mask / SSAO chain / PSSM pass and the terrain blur on our own shaders (native.own_post; acted = draws on the
    // effect ports + S-7c units + kernel runs in the window; compare counts as armed)
    {
        static std::uint64_t last = 0;
        const std::uint64_t acts = ownPostActs();
        const std::uint64_t a = acts >= last ? acts - last : acts;
        last = acts;
        const int want = settingInt(rs.nativeOwnPost);
        const int have = ownPostMode();
        line += stage("own_post", want != 0, have != 0 && have == want, a,
                      std::format("{}: {} effect draws / kernel runs on our shaders", have == 2 ? "compare" : "on", a));
    }
    // F12 / F13 (frame-ownership.md 14.24): the sky dome / the GUI sprite batches on our own shaders (native.own_sky / native.own_gui; acted = composed draws drawn with
    // the ports in the window; compare counts as armed)
    for (const OwnPost::Scene which : {OwnPost::Scene::Sky, OwnPost::Scene::Gui, OwnPost::Scene::Particles}) {
        static std::uint64_t last[3] = {};
        const auto i = static_cast<std::size_t>(which);
        const std::uint64_t acts = ownSceneActs(which);
        const std::uint64_t a = acts >= last[i] ? acts - last[i] : acts;
        last[i] = acts;
        static constexpr const char* kStage[3] = {"own_sky", "own_gui", "own_particles"};
        static constexpr const char* kWhat[3] = {"sky dome draws", "sprite batch draws", "particle draws (F11: quads expanded on the GPU)"};
        settings::Setting* st = which == OwnPost::Scene::Sky ? rs.nativeOwnSky : which == OwnPost::Scene::Gui ? rs.nativeOwnGui : rs.nativeOwnParticles;
        const int want = settingInt(st);
        const int have = ownSceneMode(which);
        // (14.34, owned: acted = units drawn from their records in the window: the sky dome's persistent record, sprite flushes from pulled records)
        static std::uint64_t lastOwned[3] = {};
        const std::uint64_t owned = ownSceneOwnedActs(which);
        const std::uint64_t o = owned >= lastOwned[i] ? owned - lastOwned[i] : owned;
        lastOwned[i] = owned;
        if (have == 3) {
            static constexpr const char* kOwned[3] = {"sky dome draws from the persistent record", "sprite flushes from pulled records", ""};
            line += stage(kStage[i], want != 0, have == want, o, std::format("owned: {} {}, {} {} on our shaders", o, kOwned[i], a, kWhat[i]));
            continue;
        }
        line += stage(kStage[i], want != 0, have != 0 && have == want, a, std::format("{}: {} {} on our shaders", have == 2 ? "compare" : "on", a, kWhat[i]));
    }
    // row 13 (frame-ownership.md 14.26): native.owned_decals (acted = decal records drawn from their persistent buffers / content-checked in the window)
    {
        const int want = settingInt(rs.nativeOwnedDecals);
        const std::uint64_t a = decalW_.hits + decalW_.checked;
        line += stage("owned_decals", want != 0, ownedDecals_ != 0 && ownedDecals_ == want, a,
                      std::format("{}: {} decal records from persistent buffers, {} checked", ownedDecals_ == 2 ? "compare" : "on", decalW_.hits, decalW_.checked));
    }
    // S-1a: the HLSL ubershader (debug.ubershader; the owned-shader line holds the per-kind counts)
    const bool uberSetting = settingOn(rs.ubershader), uberCode = dev_.ubershader();
    line += std::format(" ubershader={}", uberCode == uberSetting ? (uberCode ? "active(HLSL material shaders)" : "inactive(setting off)") : "INCONSISTENT(setting vs code)");
    spdlog::info("native: stages (5 s):{}", line);
}

// S-4 (frame-ownership.md 14.12): per view kind of the persistent owned flushes: items, pulled items, indirect commands (= buckets) per flush, the pulled share of them;
// and why items / layouts were not pulled. Logged while pulling is on, and once more in the window it went off
void StaticOps::vertexPullReport() {
    d3d9::Device::VertexPullStats& s = dev_.vertexPullStats();
    // (also while off: the commands per flush of the vertex-input buckets are the A/B's baseline)
    const bool on = dev_.vertexPull();
    if (s.flushes[0] + s.flushes[1] + s.flushes[2] + s.flushes[3] == 0 && !on) {
        s = {};
        return;
    }
    static constexpr const char* kKind[4] = {"main z", "main solid", "other", "cascades"};
    std::string views;
    for (int k = 0; k < 4; ++k) {
        if (s.flushes[k] == 0) continue;
        const double f = static_cast<double>(s.flushes[k]);
        views += std::format(" [{}: {} flushes, {:.1f} items / flush, {:.1f} pulled, {:.1f} indirect commands / flush ({:.1f} of them pulled; {:.1f} distinct pipelines, {:.1f} index bindings)]",
                             kKind[k], s.flushes[k], static_cast<double>(s.items[k]) / f, static_cast<double>(s.pulled[k]) / f, static_cast<double>(s.buckets[k]) / f,
                             static_cast<double>(s.pulledBuckets[k]) / f, static_cast<double>(s.pipelines[k]) / f, static_cast<double>(s.indexBindings[k]) / f);
        if (s.indices[k] != 0) {
            views.pop_back();
            views += std::format("; pulled: {:.0f} k indices / flush, {:.2f} MB fetched / flush ({:.1f} B per index, POSITION0 {:.2f} MB)]", static_cast<double>(s.indices[k]) / f / 1e3,
                                 static_cast<double>(s.vtxBytes[k]) / f / 1e6, static_cast<double>(s.vtxBytes[k]) / static_cast<double>(s.indices[k]), static_cast<double>(s.posBytes[k]) / f / 1e6);
        }
    }
    spdlog::info("native: vertex pull (5 s): {}{}; items not pulled: no pulled pipeline {}, no fetch block {}; draw layouts pullable {}, refused (format {}, instance / user-pointer stream {}, "
                 "no arena address {}, alignment {}, stream > 3 {}); pulled pipelines pending {}; variant modules (run): {} built, {} requests shared a module of the same text",
                 on ? "on" : "off", views.empty() ? std::string(" (no owned flush)") : views, s.noPipeline, s.noFetch, s.layoutOk, s.layoutRefused[0], s.layoutRefused[1], s.layoutRefused[2],
                 s.layoutRefused[3], s.layoutRefused[4], s.pipelinesPending, dev_.sharedModuleMisses(), dev_.sharedModuleHits());
    s = {};
}

}  // namespace rvk::native
