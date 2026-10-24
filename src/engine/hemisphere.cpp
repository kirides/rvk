#include "engine/hemisphere.h"

#include <cstring>
#include <mutex>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::hemisphere {
namespace {

// eSShaderMaterialLightList (FUN_1806b5bb0 / eCHemisphere_PS::AddToLightList): the SH registers at +0xb8 (7 x 16 bytes), the
// indoor ambient colour at +0x128 (3 floats); eCHemisphere_PS: intensity at +0x94, its SH (9 x vec4) at +0xdc
constexpr std::size_t kListRegs = 0xb8;
constexpr std::size_t kListIndoor = 0x128;
constexpr std::size_t kIntensity = 0x94;

using AddFn = void (*)(void* self, void* list);
Hook<AddFn> g_add;

std::mutex g_mutex;
Light g_light;
bool g_have = false;
const void* g_seen[8]{};
std::uint32_t g_seenCount = 0;

void detourAdd(void* self, void* list) {
    g_add.original(self, list);
    Light l;
    bool ok = list != nullptr && readAt(list, kListRegs, l.regs) && readAt(list, kListIndoor, l.indoor);
    if (ok) readAt(self, kIntensity, l.intensity);
    std::lock_guard lock(g_mutex);
    ++g_light.calls;
    if (!ok) return;
    bool known = false;
    for (std::uint32_t i = 0; i < g_seenCount && i < 8; ++i) known = known || g_seen[i] == self;
    if (!known) {
        if (g_seenCount < 8) g_seen[g_seenCount] = self;
        ++g_seenCount;
        spdlog::info("engine: hemisphere light {} (object {}): sky L0 ({:.3f}, {:.3f}, {:.3f}) indoor ({:.3f}, {:.3f}, {:.3f}) intensity {:.2f}",
                     g_seenCount, self, l.regs[0][3] + l.regs[3][2] / 3.0f, l.regs[1][3] + l.regs[4][2] / 3.0f,
                     l.regs[2][3] + l.regs[5][2] / 3.0f, l.indoor[0], l.indoor[1], l.indoor[2], l.intensity);
    }
    const std::uint64_t calls = g_light.calls;
    g_light = l;
    g_light.calls = calls;
    g_light.object = self;
    g_light.objects = g_seenCount;
    g_have = true;
}

}  // namespace

bool latest(Light& out) {
    std::lock_guard lock(g_mutex);
    if (!g_have) return false;
    out = g_light;
    return true;
}

bool install() {
    const auto& s = sym();
    if (s.hemisphereAddToLightList == nullptr) {
        spdlog::warn("engine: eCHemisphere_PS::AddToLightList not found: the GI sky is inferred from the draws");
        return false;
    }
    return g_add.install("eCHemisphere_PS::AddToLightList", reinterpret_cast<AddFn>(s.hemisphereAddToLightList), &detourAdd);
}

}  // namespace rvk::engine::hemisphere
