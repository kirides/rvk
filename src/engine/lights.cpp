#include "engine/lights.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/hooks.h"
#include "engine/memread.h"
#include "engine/symbols.h"

namespace rvk::engine::lights {
namespace {

// eCIlluminationAdmin: dynamic lights (eCDynamicLight_PS*) data at +0x40, count at +0x48; the static lights' sphere tree at
// +0x68 (eTSphereTree<eCStaticLight_PS*>): root index at tree +0x20, node array pointer at tree +0x30, 0x50-byte nodes:
// sphere (x, y, z, r) at +0, payload at +0x10, next sibling index at +0x28, first child index at +0x38, flags at +0x48
// (bit 4: a leaf with a light payload). Read from FUN_18014f080 / FUN_180576cb0 (FindWightedStaticLights' walk)
constexpr std::size_t kAdminDynData = 0x40, kAdminDynCount = 0x48, kAdminTree = 0x68;
constexpr std::size_t kTreeRoot = 0x20, kTreeNodes = 0x30, kNodeSize = 0x50;
constexpr std::size_t kNodeSphere = 0x0, kNodePayload = 0x10, kNodeSibling = 0x28, kNodeChild = 0x38, kNodeFlags = 0x48;
// eCStaticLight_PS: colour (rgb) at +0x30, intensity +0x40, range +0x44; position: eCStaticPointLight_PS +0x6c,
// eCStaticAmbientLight_PS +0x58. The engine's colour constant = rgb x intensity x DAT_1809f1730 (read at run time)
constexpr std::size_t kLightRgb = 0x30, kLightIntensity = 0x40, kLightRange = 0x44, kPointPos = 0x6c, kAmbientPos = 0x58;
constexpr std::size_t kScaleRva = 0x9f1730;
// eSShaderMaterialLightList: count byte +0x34, entry i at i * 0x40: position xyz +0x38, 1 / R^2 +0x44, colour +0x58
constexpr std::size_t kListCount = 0x34, kListPos = 0x38, kListInvR2 = 0x44, kListColour = 0x58;
constexpr std::uint32_t kMaxNodes = 60000;
constexpr float kDrawShare = 0.5f;

using FindFn = int (*)(void* self, void* out, unsigned long n, const void* box, void* room);
using RegisterFn = int (*)(void* self, void* light);
using AddFn = void (*)(void* self, void* list);

Hook<FindFn> g_find;
Hook<RegisterFn> g_register;
std::atomic<void*> g_admin{nullptr};
std::atomic<std::uint64_t> g_adminLogged{0};

void noteAdmin(void* self) {
    if (self == nullptr || g_admin.load(std::memory_order_relaxed) == self) return;
    g_admin.store(self, std::memory_order_relaxed);
    if (g_adminLogged.fetch_add(1) < 4) spdlog::info("engine: illumination admin {}", self);
}

int detourFind(void* self, void* out, unsigned long n, const void* box, void* room) {
    noteAdmin(self);
    return g_find.original(self, out, n, box, room);
}

int detourRegister(void* self, void* light) {
    noteAdmin(self);
    return g_register.original(self, light);
}

bool callAdd(AddFn fn, void* light, void* list) {
    __try {
        fn(light, list);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float staticScale() {
    static float scale = [] {
        float v = 1.0f;
        HMODULE m = GetModuleHandleW(L"Engine.dll");
        if (m != nullptr && !readAt(m, kScaleRva, v)) v = 1.0f;
        if (!(v > 0.0f && v < 100.0f)) v = 1.0f;
        spdlog::info("engine: static light colour scale {:.4f}", v);
        return v;
    }();
    return scale;
}

}  // namespace

bool collect(const float center[3], float radius, std::vector<WorldLight>& out, Stats& stats) {
    out.clear();
    stats = {};
    char* admin = static_cast<char*>(g_admin.load(std::memory_order_relaxed));
    if (admin == nullptr) return false;
    stats.adminSeen = true;
    const auto& s = sym();
    const auto reaches = [&](const float p[3], float r) {
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float reach = radius + r;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };

    // static lights: the sphere tree
    const void* ptfv = s.staticPointLightVftable;
    const void* ambfv = s.staticAmbientLightVftable;
    const float scale = staticScale();
    const char* tree = admin + kAdminTree;
    std::int64_t root = -1;
    const char* nodes = nullptr;
    if (readAt(tree, kTreeRoot, root) && readAt(tree, kTreeNodes, nodes) && nodes != nullptr && root >= 0) {
        std::vector<std::int64_t> stack;
        stack.reserve(256);
        stack.push_back(root);
        while (!stack.empty() && stats.nodes < kMaxNodes) {
            const std::int64_t idx = stack.back();
            stack.pop_back();
            std::int64_t child = -1;
            if (!readAt(nodes + idx * kNodeSize, kNodeChild, child)) continue;
            while (child != -1 && stats.nodes < kMaxNodes) {
                ++stats.nodes;
                const char* n = nodes + child * kNodeSize;
                std::uint8_t flags = 0;
                float sphere[4]{};
                std::int64_t next = -1, firstChild = -1;
                const void* payload = nullptr;
                if (!readAt(n, kNodeFlags, flags) || !readAt(n, kNodeSphere, sphere) || !readAt(n, kNodeSibling, next) ||
                    !readAt(n, kNodeChild, firstChild)) {
                    break;
                }
                bool descend = true;
                if ((flags & 4) != 0) {
                    if (readAt(n, kNodePayload, payload) && payload != nullptr) {
                        const void* vt = nullptr;
                        float pos[3]{}, rgb[3]{}, intensity = 0.0f, range = 0.0f;
                        if (readAt(payload, 0, vt) && (vt == ptfv || vt == ambfv) && readAt(payload, kLightRgb, rgb) &&
                            readAt(payload, kLightIntensity, intensity) && readAt(payload, kLightRange, range) &&
                            readAt(payload, vt == ptfv ? kPointPos : kAmbientPos, pos)) {
                            ++stats.staticTotal;
                            // the draws' constants carry half of rgb x intensity x scale (measured: ratio 0.49-0.50, rtgi.md 19)
                            const float k = intensity * scale * kDrawShare;
                            if (range > 0.0f && range < 1e6f && k > 0.0f && std::isfinite(k) && reaches(pos, range)) {
                                ++stats.staticNear;
                                WorldLight l;
                                std::memcpy(l.pos, pos, sizeof(pos));
                                l.range = range;
                                for (int c = 0; c < 3; ++c) l.col[c] = rgb[c] * k;
                                out.push_back(l);
                            }
                        } else {
                            ++stats.unknown;
                        }
                    }
                } else if (std::isfinite(sphere[0]) && std::isfinite(sphere[3])) {
                    descend = reaches(sphere, sphere[3]);
                }
                if (descend && firstChild != -1) stack.push_back(child);
                child = next;
            }
        }
    }

    // dynamic lights: the engine's own light list entry of each (flicker / effects included)
    const void* pfv = s.pointLightVftable;
    const auto add = reinterpret_cast<AddFn>(s.pointLightAddToLightList);
    const char* data = nullptr;
    std::uint32_t count = 0;
    if (pfv != nullptr && add != nullptr && readAt(admin, kAdminDynData, data) && readAt(admin, kAdminDynCount, count) &&
        data != nullptr && count > 0 && count < 4096) {
        stats.dynamicRaw = count;
        for (std::uint32_t i = 0; i < count; ++i) {
            void* light = nullptr;
            const void* vt = nullptr;
            if (!readAt(data, i * 8, light) || !readAt(light, 0, vt)) continue;
            if (i < 4) stats.dynamicVt[i] = reinterpret_cast<std::uintptr_t>(vt);
            if (vt != pfv) continue;
            ++stats.dynamicTotal;
            alignas(16) char list[0x200]{};
            if (!callAdd(add, light, list)) continue;
            if (list[kListCount] == 0) continue;  // disabled
            float pos[3]{}, col[3]{}, invR2 = 0.0f;
            std::memcpy(pos, list + kListPos, sizeof(pos));
            std::memcpy(&invR2, list + kListInvR2, sizeof(invR2));
            std::memcpy(col, list + kListColour, sizeof(col));
            if (!(invR2 > 0.0f) || !std::isfinite(invR2) || !(col[0] + col[1] + col[2] > 0.0f)) continue;
            const float range = 1.0f / std::sqrt(invR2);
            if (!reaches(pos, range)) continue;
            ++stats.dynamicNear;
            WorldLight l;
            std::memcpy(l.pos, pos, sizeof(pos));
            l.range = range;
            std::memcpy(l.col, col, sizeof(col));
            l.dynamic = true;
            out.push_back(l);
        }
    }
    return true;
}

bool install() {
    const auto& s = sym();
    bool ok = false;
    if (s.illuminationFindWighted != nullptr) {
        ok = g_find.install("eCIlluminationAdmin::FindWightedStaticLights", reinterpret_cast<FindFn>(s.illuminationFindWighted), &detourFind) || ok;
    }
    if (s.illuminationRegisterDynamic != nullptr) {
        ok = g_register.install("eCIlluminationAdmin::RegisterDynamicLightPS", reinterpret_cast<RegisterFn>(s.illuminationRegisterDynamic),
                                &detourRegister) || ok;
    }
    if (!ok) spdlog::warn("engine: the illumination admin's symbols were not found: the GI sees only the drawn objects' lights");
    return ok;
}

}  // namespace rvk::engine::lights
