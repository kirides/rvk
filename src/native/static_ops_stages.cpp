// The owned stages' state line (docs/research/frame-ownership.md 7.47): one line per 5 s window that names, for every stage of the frame-ownership
// family, whether its SETTING is on, whether the code is armed, and whether it ACTED in the window. The A/B scripts (scripts/lib/stage-state.ps1) read it:
// a toggle that does not apply (7.46: `set native.owned_trees` was never wired to applyStaticOps) shows as `inactive(setting on, code off)`.

#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "core/settings.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/owned_compact.h"
#include "native/static_ops.h"

namespace rvk::native {

namespace {

bool settingOn(const settings::Setting* s) { return s != nullptr && s->asBool(); }
// (restart settings: what the session started with is what runs)
bool startOn(const settings::Setting* s) { return s != nullptr && s->startValue() != 0.0; }

std::string stage(const char* name, bool setting, bool armed, std::uint64_t acted, const std::string& detail) {
    std::string state;
    if (!setting && !armed) state = "inactive(setting off)";
    else if (!setting && armed) state = std::format("INCONSISTENT(setting off, code on, acted {})", acted);
    else if (!armed) state = "inactive(setting on, code off)";
    else if (acted == 0) state = "inactive(armed, nothing owned in the window)";
    else state = std::format("active({})", detail);
    return std::format(" {}={}", name, state);
}

}  // namespace

void StaticOps::stagesReport() {
    const auto& rs = settings::renderer();
    static std::uint64_t lastCullJobs = 0;
    gpuscene::OwnedCompact::Stats& cs = gpuscene::ownedcompact().stats();
    const std::uint64_t cullJobs = cs.cullJobs - lastCullJobs;
    lastCullJobs = cs.cullJobs;
    std::string line;
    line += stage("owned_static", settingOn(rs.nativeOwnedStatic), ownedStatic_, ownedW_.owned, std::format("{} ops owned in {} views", ownedW_.owned, ownedW_.views));
    line += stage("owned_persistent", settingOn(rs.nativeOwnedPersistent), ownedPersistent_, persistW_.passes, std::format("{} passes", persistW_.passes));
    const std::uint64_t treeMain = tro_ ? tro_->window.ownedViews : 0, treeCasc = tro_ ? tro_->shadowWindow.ownedViews : 0;
    line += stage("owned_trees", settingOn(rs.nativeOwnedTrees), ownedTrees_, treeMain + treeCasc, std::format("main {} owned views, cascades {}", treeMain, treeCasc));
    line += stage("owned_shadows", settingOn(rs.nativeOwnedShadows), ownedShadowsOn(), ownedShadowW_.owned, std::format("{} cascade ops owned", ownedShadowW_.owned));
    line += stage("owned_actors", settingOn(rs.nativeOwnedActors), ownedActorsOn(), actorW_.drawn, std::format("{} draws", actorW_.drawn));
    line += stage("owned_vegetation", settingOn(rs.nativeOwnedVegetation), ownedVeg_, vegStat_.retained, std::format("{} ops on retained streams", vegStat_.retained));
    const std::uint64_t vegViews = vro_ ? vro_->window.ownedViews : 0;
    line += stage("owned_vegetation_ops", settingOn(rs.nativeOwnedVegetationOps), ownedVegOps_, vegViews, std::format("{} owned views", vegViews));
    line += stage("clusters", startOn(rs.nativeClusters), gpuscene::clusterstore().active(), gpuscene::clusterstore().active() ? 1 : 0, "store active");
    line += stage("cluster_cull", startOn(rs.nativeClusterCull), dev_.clusterCullOn(), cullJobs, std::format("{} jobs with the cull", cullJobs));
    // S-1a: the HLSL ubershader (debug.ubershader; the owned-shader line holds the per-kind counts)
    const bool uberSetting = settingOn(rs.ubershader), uberCode = dev_.ubershader();
    line += std::format(" ubershader={}", uberCode == uberSetting ? (uberCode ? "active(HLSL material shaders)" : "inactive(setting off)") : "INCONSISTENT(setting vs code)");
    spdlog::info("native: stages (5 s):{}", line);
}

}  // namespace rvk::native
