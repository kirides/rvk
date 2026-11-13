#include "engine/lights.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <mutex>

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

namespace {

// The static lights change only when the engine caches a cell in or out: the walk of the sphere tree (a few thousand SEH-guarded node reads, 0.1 - 0.35 ms
// in a town) is kept between calls and re-run when the tree's own header changes (root / node array / node count words at +0x20, +0x30, +0x38), the query
// leaves the walked sphere, or every kRewalkCalls calls (a safety net for an in-place change); each call filters the kept list by its own centre and
// radius (same lights, same order as a walk with that radius: the walk visits in tree order and only prunes).
struct StaticCache {
    std::vector<const char*> lights;  // the payload of every static light within `radius` of `center` at walk time; its values are re-read on every call
    float center[3]{};
    float radius = 0.0f;             // the walk's radius (query radius + margin)
    std::uint64_t sig[3]{};
    std::uint32_t calls = 0;         // calls served since the walk
    std::uint64_t walks = 0;
    Stats walkStats;                 // nodes / staticTotal / unknown of the walk
    bool valid = false;
};
StaticCache g_cache;
std::mutex g_cacheMutex;
constexpr std::uint32_t kRewalkCalls = 150;
constexpr float kWalkMargin = 4000.0f;

// One static light's current values (one guarded read of the object's first 0x78 bytes: vtable, colour, intensity, range, both position slots).
// 0 = not a readable static light, 1 = readable but not usable (range / intensity), 2 = usable
int readStaticLight(const void* payload, const void* ptfv, const void* ambfv, float scale, WorldLight& l) {
    alignas(8) std::uint8_t b[0x78];
    if (payload == nullptr || !safeReadN<sizeof(b)>(payload, b)) return 0;
    const void* vt;
    std::memcpy(&vt, b, sizeof(vt));
    if (vt != ptfv && vt != ambfv) return 0;
    float rgb[3], intensity, range, pos[3];
    std::memcpy(rgb, b + kLightRgb, sizeof(rgb));
    std::memcpy(&intensity, b + kLightIntensity, sizeof(intensity));
    std::memcpy(&range, b + kLightRange, sizeof(range));
    std::memcpy(pos, b + (vt == ptfv ? kPointPos : kAmbientPos), sizeof(pos));
    // the draws' constants carry half of rgb x intensity x scale (measured: ratio 0.49-0.50, rtgi.md 19)
    const float k = intensity * scale * kDrawShare;
    if (!(range > 0.0f && range < 1e6f && k > 0.0f && std::isfinite(k))) return 1;
    l = WorldLight{};
    std::memcpy(l.pos, pos, sizeof(pos));
    l.range = range;
    l.ambient = vt == ambfv;
    for (int c = 0; c < 3; ++c) l.col[c] = rgb[c] * k;
    return 2;
}

void walkStatics(const char* admin, const float center[3], float radius, StaticCache& c) {
    c.lights.clear();
    c.walkStats = {};
    Stats& stats = c.walkStats;
    const auto& s = sym();
    const auto reaches = [&](const float p[3], float r) {
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        const float reach = radius + r;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };
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
                // the node's 80 bytes in one guarded copy (one element of the node array: always whole)
                alignas(8) std::uint8_t nb[kNodeSize];
                if (!safeReadN<kNodeSize>(n, nb)) break;
                const std::uint8_t flags = nb[kNodeFlags];
                float sphere[4];
                std::int64_t next, firstChild;
                const void* payload;
                std::memcpy(sphere, nb + kNodeSphere, sizeof(sphere));
                std::memcpy(&next, nb + kNodeSibling, sizeof(next));
                std::memcpy(&firstChild, nb + kNodeChild, sizeof(firstChild));
                std::memcpy(&payload, nb + kNodePayload, sizeof(payload));
                bool descend = true;
                if ((flags & 4) != 0) {
                    if (payload != nullptr) {
                        WorldLight l;
                        const int r = readStaticLight(payload, ptfv, ambfv, scale, l);
                        if (r == 0) {
                            ++stats.unknown;
                        } else {
                            ++stats.staticTotal;
                            if (r == 2 && reaches(l.pos, l.range)) c.lights.push_back(static_cast<const char*>(payload));
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

    // static lights: the sphere tree (walked only when it changed, see StaticCache)
    {
        const std::lock_guard lock(g_cacheMutex);
        StaticCache& c = g_cache;
        const char* tree = admin + kAdminTree;
        std::uint64_t sig[3]{};
        readAt(tree, kTreeRoot, sig[0]);
        readAt(tree, kTreeNodes, sig[1]);
        readAt(tree, 0x38, sig[2]);
        const float dx = center[0] - c.center[0], dy = center[1] - c.center[1], dz = center[2] - c.center[2];
        const bool inside = c.valid && std::sqrt(dx * dx + dy * dy + dz * dz) + radius <= c.radius;
        const bool reuse = inside && std::memcmp(sig, c.sig, sizeof(sig)) == 0 && c.calls < kRewalkCalls;
        if (!reuse) {
            // the walk's radius never shrinks (the GI asks for 16,000, the light table for 96,000: one walk serves both)
            const float want = radius + kWalkMargin;
            const float walkRadius = c.valid ? std::max(want, c.radius) : want;
            walkStatics(admin, center, walkRadius, c);
            std::memcpy(c.center, center, sizeof(c.center));
            c.radius = walkRadius;
            std::memcpy(c.sig, sig, sizeof(sig));
            c.calls = 0;
            c.valid = true;
            ++c.walks;
        }
        ++c.calls;
        stats.nodes = c.walkStats.nodes;
        stats.staticTotal = c.walkStats.staticTotal;
        stats.unknown = c.walkStats.unknown;
        out.reserve(c.lights.size());
        const float scale = staticScale();
        const void* ptfv = s.staticPointLightVftable;
        const void* ambfv = s.staticAmbientLightVftable;
        for (const char* payload : c.lights) {
            WorldLight l;
            if (readStaticLight(payload, ptfv, ambfv, scale, l) != 2 || !reaches(l.pos, l.range)) continue;
            ++stats.staticNear;
            out.push_back(l);
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

// The pre-cache static walk, verbatim (RVK_LIGHTS_VERIFY compares the cached path against it)
bool collectStaticsReference(const float center[3], float radius, std::vector<WorldLight>& out) {
    out.clear();
    Stats stats;
    char* admin = static_cast<char*>(g_admin.load(std::memory_order_relaxed));
    if (admin == nullptr) return false;
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
                // the node's 80 bytes in one guarded copy (one element of the node array: always whole)
                alignas(8) std::uint8_t nb[kNodeSize];
                if (!safeReadN<kNodeSize>(n, nb)) break;
                const std::uint8_t flags = nb[kNodeFlags];
                float sphere[4];
                std::int64_t next, firstChild;
                const void* payload;
                std::memcpy(sphere, nb + kNodeSphere, sizeof(sphere));
                std::memcpy(&next, nb + kNodeSibling, sizeof(next));
                std::memcpy(&firstChild, nb + kNodeChild, sizeof(firstChild));
                std::memcpy(&payload, nb + kNodePayload, sizeof(payload));
                bool descend = true;
                if ((flags & 4) != 0) {
                    if (payload != nullptr) {
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
                                l.ambient = vt == ambfv;
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
