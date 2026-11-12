#include "engine/illum_census.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "core/settings.h"
#include "engine/symbols.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/lighting_images.h"

namespace rvk::engine::illumcensus {

bool g_on = false;
bool g_table = false;

namespace {

constexpr std::size_t kDirtyByte = 0x188, kPsSet = 0x180, kIllumF = 0x08;

using VoidFn = void (*)(void*);
using BoolFn = bool (*)(void*);
using SetFn = void (*)(void*, void*);
Hook<BoolFn> g_setUpdate;
Hook<VoidFn> g_psUpdate, g_admin, g_psDestroy;
Hook<SetFn> g_setLightSet;
Hook<VoidFn> g_dynLightUpdate;

using Ptrs = std::unordered_set<const void*>;

struct Frame {
    std::uint32_t setUpdates = 0, psUpdates = 0, psDirty = 0, setCalls = 0, setChanged = 0;
    std::uint32_t drawLights = 0, treeLights = 0, vegLights = 0;  // writeLighting calls
    std::uint32_t fChanged = 0, fSeen = 0;                        // outdoor factor of the distinct drawn illuminations: changed since the last frame / compared
    Ptrs updatedSets, drawnIllum, drawnList, drawnTreeList, drawnTreeIllum, drawnVegList;
};

struct Window {
    std::uint64_t frames = 0;
    // sums and maxima of every Frame field (distinct counts = set sizes)
    double sum[14]{};
    std::uint32_t max[14]{};
    void add(int i, std::uint32_t v) {
        sum[i] += v;
        max[i] = std::max(max[i], v);
    }
};

std::mutex g_mutex;
Frame g_f;
Window g_w;
int g_kind = 0;
std::uint64_t g_frameNo = 0;
std::unordered_map<const void*, float> g_lastF;
Ptrs g_runSets, g_runIllum, g_runList, g_runTreeList, g_runVegList;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

const char* const kNames[14] = {"set updates",     "distinct sets updated", "PS updates",      "PS dirty",         "SetLightSet calls",
                                "SetLightSet changed", "lit draw passes",     "distinct illums",  "distinct lists",   "tree lists",
                                "tree illums",     "vegetation lists",   "f changed",        "f compared"};

void endFrame() {
    const Frame& f = g_f;
    std::uint32_t v[14] = {f.setUpdates,  static_cast<std::uint32_t>(f.updatedSets.size()),
                           f.psUpdates,   f.psDirty,
                           f.setCalls,    f.setChanged,
                           f.drawLights,  static_cast<std::uint32_t>(f.drawnIllum.size()),
                           static_cast<std::uint32_t>(f.drawnList.size()), static_cast<std::uint32_t>(f.drawnTreeList.size()),
                           static_cast<std::uint32_t>(f.drawnTreeIllum.size()), static_cast<std::uint32_t>(f.drawnVegList.size()),
                           f.fChanged,    f.fSeen};
    for (int i = 0; i < 14; ++i) g_w.add(i, v[i]);
    ++g_w.frames;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last >= std::chrono::seconds(5) && g_w.frames > 0) {
        std::string s;
        for (int i = 0; i < 14; ++i) {
            s += std::format("{}{} {:.1f}/{}", i == 0 ? "" : ", ", kNames[i], g_w.sum[i] / static_cast<double>(g_w.frames), g_w.max[i]);
        }
        spdlog::info("engine: illum census (5 s): {} frames; avg/max per frame: {}; tree draw passes {}, vegetation draw passes {}; run distinct: sets {}, illums {}, lists {}, tree lists {}, veg lists {}",
                     g_w.frames, s, f.treeLights, f.vegLights, g_runSets.size(), g_runIllum.size(), g_runList.size(), g_runTreeList.size(), g_runVegList.size());
        g_w = Window{};
        g_last = now;
    }
    g_f = Frame{};
    ++g_frameNo;
}

void detourAdmin(void* self) {
    if (g_on) {
        std::lock_guard lock(g_mutex);
        endFrame();  // the previous frame's updates and draws
    }
    g_admin.original(self);
    if (g_table) gpuscene::illumtable().onSetsUpdated();
}

bool detourSetUpdate(void* self) {
    if (g_on) {
        std::lock_guard lock(g_mutex);
        ++g_f.setUpdates;
        g_f.updatedSets.insert(self);
        g_runSets.insert(self);
    }
    const bool r = g_setUpdate.original(self);
    if (g_table) gpuscene::illumtable().onSetUpdated(self);
    return r;
}

// eCDynamicLight_PS::UpdateIllumination (a dynamic light moved / changed: OnUpdatedWorldMatrix): rebuilds the lists of the light's sets in place with its own
// inlined loop (no eCLightSet::UpdateIllumination call). Without this hook a moving light (the light spell) left the set records at the bytes of the last
// UpdateLightSets while the frame's draws read the moved light: table / lighting images DIFFERING, owned draws lit from the stale position (14.30)
void detourDynLightUpdate(void* self) {
    g_dynLightUpdate.original(self);
    if (!g_table || self == nullptr) return;
    constexpr std::size_t kSets = 0x30, kSetCount = 0x38;
    const std::uint8_t* const* sets = nullptr;
    std::int32_t n = 0;
    if (!readAtSafe(self, kSets, sets) || !readAtSafe(self, kSetCount, n) || sets == nullptr || n <= 0 || n > 64) return;
    for (std::int32_t i = 0; i < n; ++i) {
        const void* set = nullptr;
        if (readAtSafe(sets, static_cast<std::size_t>(i) * sizeof(void*), set) && set != nullptr) gpuscene::illumtable().onSetUpdated(set, true);
    }
}

void detourPsUpdate(void* self) {
    std::uint8_t dirty = 0;
    readAt(self, kDirtyByte, dirty);
    if (g_on) {
        std::lock_guard lock(g_mutex);
        ++g_f.psUpdates;
        if (dirty != 0) ++g_f.psDirty;
    }
    g_psUpdate.original(self);
    if (g_table) gpuscene::illumtable().onPsUpdate(self, dirty != 0);
}

void detourPsDestroy(void* self) {
    if (g_table) gpuscene::illumtable().onPsDestroy(self);
    g_psDestroy.original(self);
}

void detourSetLightSet(void* self, void* set) {
    const void* old = nullptr;
    readAt(self, kPsSet, old);
    if (g_on) {
        std::lock_guard lock(g_mutex);
        ++g_f.setCalls;
        if (old != set) ++g_f.setChanged;
    }
    g_setLightSet.original(self, set);
    if (g_table) gpuscene::illumtable().onSetLightSet(self);
}

}  // namespace

void setKind(int kind) {
    std::lock_guard lock(g_mutex);
    g_kind = kind;
}

void noteDrawImpl(const void* illum, const void* list) {
    float f = 0.0f;
    const bool haveF = readAt(illum, kIllumF, f);
    std::lock_guard lock(g_mutex);
    ++g_f.drawLights;
    if (g_kind == 1) ++g_f.treeLights;
    if (g_kind == 2) ++g_f.vegLights;
    if (g_f.drawnIllum.insert(illum).second) {
        g_runIllum.insert(illum);
        if (haveF) {
            const auto it = g_lastF.find(illum);
            if (it != g_lastF.end()) {
                ++g_f.fSeen;
                if (it->second != f) ++g_f.fChanged;
                it->second = f;
            } else {
                if (g_lastF.size() > 100000) g_lastF.clear();
                g_lastF.emplace(illum, f);
            }
        }
    }
    g_f.drawnList.insert(list);
    g_runList.insert(list);
    if (g_kind == 1) {
        g_f.drawnTreeList.insert(list);
        g_f.drawnTreeIllum.insert(illum);
        g_runTreeList.insert(list);
    } else if (g_kind == 2) {
        g_f.drawnVegList.insert(list);
        g_runVegList.insert(list);
    }
}

bool install() {
    char buf[8]{};
    const bool census = GetEnvironmentVariableA("RVK_ILLUM_CENSUS", buf, sizeof(buf)) != 0 && buf[0] == '1';
    const auto* setting = settings::renderer().nativeIllumTable;
    const bool table = setting != nullptr && setting->asBool() && settings::startedFull();  // (S-9: default on, Full sessions only)
    if (!census && !table) return false;
    const auto& s = sym();
    bool ok = true;
    ok &= g_setUpdate.install("eCLightSet::UpdateIllumination", reinterpret_cast<BoolFn>(s.lightSetUpdateIllumination), &detourSetUpdate);
    ok &= g_psUpdate.install("eCIlluminated_PS::UpdateIllumination", reinterpret_cast<VoidFn>(s.illumPsUpdateIllumination), &detourPsUpdate);
    ok &= g_setLightSet.install("eCIlluminated_PS::SetLightSet", reinterpret_cast<SetFn>(s.illumPsSetLightSet), &detourSetLightSet);
    ok &= g_admin.install("eCIlluminationAdmin::UpdateLightSets", reinterpret_cast<VoidFn>(s.illumAdminUpdateLightSets), &detourAdmin);
    // RVK_DYNLIGHT_HOOK=0: the hook left out (A/B of the 14.30 defect: moving lights' set rebuilds unseen)
    char dl[8]{};
    const bool dynHook = !(GetEnvironmentVariableA("RVK_DYNLIGHT_HOOK", dl, sizeof(dl)) != 0 && dl[0] == '0');
    if (!dynHook) spdlog::warn("engine: illum table: the eCDynamicLight_PS::UpdateIllumination hook is left out (RVK_DYNLIGHT_HOOK=0)");
    if (table && dynHook) ok &= g_dynLightUpdate.install("eCDynamicLight_PS::UpdateIllumination", reinterpret_cast<VoidFn>(s.dynLightUpdateIllumination), &detourDynLightUpdate);
    if (table) ok &= g_psDestroy.install("eCIlluminated_PS::Destroy", reinterpret_cast<VoidFn>(s.illumPsDestroy), &detourPsDestroy);
    g_on = ok && census;
    g_table = ok && table;
    gpuscene::illumtable().setActive(g_table);
    if (const auto* li = settings::renderer().nativeLightingImages; li != nullptr && li->asBool() && g_table) {
        gpuscene::lightingimages().setActive(true);
        spdlog::info("native: lighting images on (native.lighting_images)");
    }
    if (census) spdlog::info("engine: illum census {} (RVK_ILLUM_CENSUS=1)", ok ? "installed" : "NOT installed: a symbol is missing");
    if (table) spdlog::info("engine: illum table hooks {} (native.illum_table)", ok ? "installed" : "NOT installed: a symbol is missing");
    return ok;
}

}  // namespace rvk::engine::illumcensus
